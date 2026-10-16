//go:build linux && (amd64 || arm64)

package netring

import (
	"encoding/binary"
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"sync/atomic"
	"time"
	"unsafe"
)

type Stats struct {
	Enters, OverflowFlushes, BufferExhaustions uint64
	FixedBytes, SendMappedBytes                int
}
type Ring struct {
	closeErr                                error
	fd                                      int
	direct                                  bool
	directSlots                             int
	control, sq, cq, sqes, bufRing, buffers []byte
	p                                       params
	pending                                 []sqe
	events                                  []Completion
	owned                                   []bool
	available                               int
	bufTail                                 uint16
	sends                                   map[uint64][]byte
	sendFree                                [][]byte
	stats                                   Stats
}

func anon(n int) ([]byte, error) {
	return unix.Mmap(-1, 0, n, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_PRIVATE|unix.MAP_ANONYMOUS)
}

// addr is used only on nonempty, live mmap slices. The Ring retains every map
// until synchronous cancellation has retired kernel references; mmap addresses
// are outside the Go heap and do not require runtime.Pinner or KeepAlive.
func addr(b []byte) uint64 { return uint64(uintptr(unsafe.Pointer(&b[0]))) }

// u32 addresses are aligned UAPI-provided offsets in a live mapping. Atomic
// acquire/release operations pair with the kernel's ring publications (ARM64 too).
func u32(b []byte, off uint32) *uint32 { return (*uint32)(unsafe.Pointer(&b[int(off)])) }
func load(b []byte, off uint32) uint32 { return atomic.LoadUint32(u32(b, off)) }
func store(b []byte, off, n uint32)    { atomic.StoreUint32(u32(b, off), n) }
func (r *Ring) register(op uint32, off, n uintptr) error {
	_, _, e := unix.Syscall6(unix.SYS_IO_URING_REGISTER, uintptr(r.fd), uintptr(op), uintptr(addr(r.control))+off, n, 0, 0)
	if e != 0 {
		return e
	}
	return nil
}
func Open(entries, buffers int) (r *Ring, err error) {
	if entries < 8 || entries > 32768 || entries&(entries-1) != 0 || buffers < 2 || buffers > 32768 || buffers&(buffers-1) != 0 {
		return nil, errors.New("ring/buffer counts must be powers of two")
	}
	r = &Ring{fd: -1, sends: make(map[uint64][]byte), owned: make([]bool, buffers), available: buffers}
	defer func() {
		if err != nil {
			err = errors.Join(err, r.Close())
			r = nil
		}
	}()
	if r.control, err = anon(4096); err != nil {
		return
	}
	// Typed overlays contain scalars only, at aligned fixed offsets in control.
	p := (*params)(unsafe.Pointer(&r.control[0]))
	p.Flags = setupCQSize | setupSingleIssuer | setupDefer | setupCoop
	p.CQEntries = uint32(entries * 4)
	fd, _, e := unix.Syscall(unix.SYS_IO_URING_SETUP, uintptr(entries), uintptr(addr(r.control)), 0)
	if e != 0 {
		err = fmt.Errorf("io_uring_setup: %w", e)
		return
	}
	r.fd = int(fd)
	r.p = *p
	required := uint32(featSingleMmap | featNodrop | featFastPoll | featExtArg)
	if p.Features&required != required {
		err = fmt.Errorf("missing ring features: got %#x need %#x", p.Features, required)
		return
	}
	sqSize := int(p.SQ.Array + p.SQEntries*4)
	cqSize := int(p.CQ.CQEs + p.CQEntries*16)
	if r.sq, err = unix.Mmap(r.fd, 0, max(sqSize, cqSize), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED); err != nil {
		return
	}
	r.cq = r.sq
	if r.sqes, err = unix.Mmap(r.fd, 0x10000000, int(p.SQEntries)*64, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED); err != nil {
		return
	}
	// Probe actual opcode support in addition to setup flags. Multishot variants
	// are tested by Probe below because REGISTER_PROBE cannot distinguish them.
	if err = r.register(8, 512, 64); err != nil {
		return
	}
	for _, op := range []uint8{opPollAdd, opAccept, opRecv, opSend, opCancel} {
		found := false
		for i := 0; i < int(r.control[513]); i++ {
			base := 528 + i*8
			if r.control[base] == op && binary.LittleEndian.Uint16(r.control[base+2:])&1 != 0 {
				found = true
			}
		}
		if !found {
			err = fmt.Errorf("io_uring opcode %d unsupported", op)
			return
		}
	}
	reg := (*bufReg)(unsafe.Pointer(&r.control[128]))
	reg.Entries = uint32(buffers)
	reg.Group = 1
	reg.Flags = 1 // IOU_PBUF_RING_MMAP, available in 6.8.
	if err = r.register(22, 128, 1); err == unix.EINVAL {
		// Ubuntu 6.8.0-136 through -147 inverted the reserved-byte check
		// (Launchpad #2162843). Retry once only after the valid UAPI request
		// fails. The same kernel bug affects unregister/status, so neither is
		// called: closing the ring releases its provided buffers.
		reg.Reserved[0] = 1
		err = r.register(22, 128, 1)
	}
	if err != nil {
		err = fmt.Errorf("provided buffer ring: %w", err)
		return
	}
	if r.bufRing, err = unix.Mmap(r.fd, 0x80000000|(1<<16), buffers*16, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED); err != nil {
		return
	}
	if r.buffers, err = anon(buffers * bufferSize); err != nil {
		return
	}
	for i := 0; i < buffers; i++ {
		r.provide(uint16(i))
	}
	for _, b := range [][]byte{r.control, r.sq, r.sqes, r.bufRing, r.buffers} {
		page := unix.Getpagesize()
		r.stats.FixedBytes += (len(b) + page - 1) / page * page
	}
	return
}
func (r *Ring) provide(id uint16) {
	slot := int(r.bufTail) & (len(r.owned) - 1)
	b := r.bufRing[slot*16:]
	binary.LittleEndian.PutUint64(b, addr(r.buffers[int(id)*bufferSize:]))
	binary.LittleEndian.PutUint32(b[8:], bufferSize)
	binary.LittleEndian.PutUint16(b[12:], id)
	r.bufTail++
	// io_uring_buf_ring.tail overlays entry zero's reserved uint16 at offset 14.
	// Publish with one aligned release store, preserving entry zero's buffer ID.
	store(r.bufRing, 12, uint32(binary.LittleEndian.Uint16(r.bufRing[12:]))|uint32(r.bufTail)<<16)
}
func (r *Ring) Recv(fd int, tag uint64) {
	flags := uint8(1 << 5)
	if r.direct {
		flags |= fixedFileFlag
	}
	r.pending = append(r.pending, sqe{Opcode: opRecv, Flags: flags, Priority: 1 << 1, FD: int32(fd), Group: 1, Tag: tag})
}
func (r *Ring) Accept(fd int) {
	flags := uint32(unix.SOCK_NONBLOCK | unix.SOCK_CLOEXEC)
	index := uint32(0)
	if r.direct {
		flags = unix.SOCK_NONBLOCK
		index = fileIndexAlloc
	}
	r.pending = append(r.pending, sqe{Opcode: opAccept, Priority: 1, FD: int32(fd), OpFlags: flags, FileIndex: index, Tag: AcceptTag})
}

// RegisterDirect creates an empty private file table. The two control overlays
// contain only scalars and live until register returns; the kernel copies them
// synchronously. All later fixed-file SQEs refer to indexes, never Go pointers.
func (r *Ring) RegisterDirect(slots int) error {
	if r.direct || slots < 1 || slots > int(^uint32(0)) {
		return errors.New("invalid direct table size")
	}
	clear(r.control[384:432])
	reg := (*rsrcRegister)(unsafe.Pointer(&r.control[384]))
	reg.NR, reg.Flags = uint32(slots), 1 // IORING_RSRC_REGISTER_SPARSE
	if e := r.register(13, 384, unsafe.Sizeof(*reg)); e != nil {
		return fmt.Errorf("sparse fixed-file registration: %w", e)
	}
	rangeArg := (*fileIndexRange)(unsafe.Pointer(&r.control[416]))
	rangeArg.Length = uint32(slots)
	// Kernel register.c requires nr_args=0 for this opcode (the man page says 1).
	if e := r.register(25, 416, 0); e != nil {
		return fmt.Errorf("fixed-file allocation range: %w", e)
	}
	r.direct, r.directSlots = true, slots
	return nil
}

func (r *Ring) Direct() bool     { return r.direct }
func (r *Ring) DirectSlots() int { return r.directSlots }
func (r *Ring) CloseDirect(slot int, tag uint64) {
	if !r.direct || slot < 0 || slot >= r.directSlots {
		panic("invalid direct close slot")
	}
	// CLOSE takes a one-based file_index and must NOT carry IOSQE_FIXED_FILE.
	r.pending = append(r.pending, sqe{Opcode: opClose, FileIndex: uint32(slot + 1), Tag: tag})
}
func (r *Ring) ShutdownDirect(slot, how int, tag uint64) {
	if !r.direct || slot < 0 || slot >= r.directSlots {
		panic("invalid direct shutdown slot")
	}
	r.pending = append(r.pending, sqe{Opcode: opShutdown, Flags: fixedFileFlag, FD: int32(slot), Len: uint32(how), Tag: tag})
}

// Poll is one-shot. The owner drains the eventfd and rearms after its CQE.
func (r *Ring) Poll(fd int, events int16, tag uint64) {
	r.pending = append(r.pending, sqe{Opcode: opPollAdd, FD: int32(fd), OpFlags: uint32(uint16(events)), Tag: tag})
}
func (r *Ring) Cancel(target, tag uint64) {
	r.pending = append(r.pending, sqe{Opcode: opCancel, FD: -1, Addr: target, Tag: tag})
}

// Send copies the response to stable memory, retaining it through its CQE even
// after client close. The caller MUST enforce one send per socket at a time.
func (r *Ring) Send(fd int, b []byte, tag uint64) (int, error) {
	if _, ok := r.sends[tag]; ok {
		return 0, errors.New("send token already in flight")
	}
	size := 4096
	for size < len(b) {
		size *= 2
	}
	var slab []byte
	for i, s := range r.sendFree {
		if len(s) >= size {
			slab = s
			r.sendFree = append(r.sendFree[:i], r.sendFree[i+1:]...)
			break
		}
	}
	if slab == nil {
		var e error
		slab, e = anon(size)
		if e != nil {
			return 0, e
		}
		r.stats.SendMappedBytes += len(slab)
	}
	copy(slab, b)
	r.sends[tag] = slab
	flags := uint8(0)
	if r.direct {
		flags = fixedFileFlag
	}
	r.pending = append(r.pending, sqe{Opcode: opSend, Flags: flags, FD: int32(fd), Addr: addr(slab), Len: uint32(len(b)), OpFlags: unix.MSG_NOSIGNAL, Tag: tag})
	return len(b), nil
}
func (r *Ring) FinishSend(tag uint64) {
	b, ok := r.sends[tag]
	if !ok {
		panic("unknown send completion")
	}
	delete(r.sends, tag)
	if len(b) <= 65536 && len(r.sendFree) < 64 {
		r.sendFree = append(r.sendFree, b)
	} else {
		unix.Munmap(b)
		r.stats.SendMappedBytes -= len(b)
	}
}
func (r *Ring) Bytes(c Completion) []byte {
	id := int(c.Flags >> 16)
	if !c.HasBuffer() || id >= len(r.owned) || !r.owned[id] || c.Result < 0 || c.Result > bufferSize {
		panic("invalid provided buffer completion")
	}
	return r.buffers[id*bufferSize : id*bufferSize+int(c.Result)]
}
func (r *Ring) Release(c Completion) {
	id := int(c.Flags >> 16)
	if !c.HasBuffer() || id >= len(r.owned) || !r.owned[id] {
		panic("provided buffer double return")
	}
	r.owned[id] = false
	r.available++
	r.provide(uint16(id))
}
func (r *Ring) Available() int { return r.available }
func (r *Ring) Stats() Stats   { return r.stats }

// Wait performs exactly one submit-and-wait enter. SQ spillover remains queued
// for the next iteration. EXT_ARG supplies the parent's 100ms timer/shutdown cap
// without allocating/submitting a timeout operation on every request.
func (r *Ring) Wait(timeout time.Duration) ([]Completion, error) {
	head := load(r.sq, r.p.SQ.Head)
	tail := load(r.sq, r.p.SQ.Tail)
	mask := load(r.sq, r.p.SQ.Mask)
	n := min(len(r.pending), int(r.p.SQEntries-(tail-head)))
	for i := 0; i < n; i++ {
		slot := (tail + uint32(i)) & mask
		// SQE overlay is 64-byte aligned/strided, contains no Go pointers, and is
		// written only after SQ head proves the kernel consumed the prior instance.
		*(*sqe)(unsafe.Pointer(&r.sqes[int(slot)*64])) = r.pending[i]
		binary.LittleEndian.PutUint32(r.sq[int(r.p.SQ.Array+slot*4):], slot)
	}
	if n > 0 {
		copy(r.pending, r.pending[n:])
		r.pending = r.pending[:len(r.pending)-n]
		store(r.sq, r.p.SQ.Tail, tail+uint32(n))
	}
	arg := (*enterArg)(unsafe.Pointer(&r.control[256]))
	ts := (*timespec)(unsafe.Pointer(&r.control[288]))
	ts.Sec = int64(timeout / time.Second)
	ts.Nsec = int64(timeout % time.Second)
	arg.TS = addr(r.control[288:])
	minimum := uintptr(1)
	if len(r.pending) > 0 || load(r.cq, r.p.CQ.Tail) != load(r.cq, r.p.CQ.Head) {
		minimum = 0
	}
	if load(r.sq, r.p.SQ.Flags)&sqOverflow != 0 {
		r.stats.OverflowFlushes++
		minimum = 0
	}
	submit := load(r.sq, r.p.SQ.Tail) - load(r.sq, r.p.SQ.Head)
	r.stats.Enters++
	_, _, e := unix.Syscall6(unix.SYS_IO_URING_ENTER, uintptr(r.fd), uintptr(submit), minimum, 1|8, uintptr(addr(r.control))+256, 24)
	if e != 0 && e != unix.EINTR && e != unix.ETIME && e != unix.EBUSY {
		return nil, fmt.Errorf("io_uring_enter: %w", e)
	}
	// NODROP overflow uses the kernel backlog, flushed by GETEVENTS next turn.
	// A nonzero cq_overflow counter means completions were LOST, not just queued.
	if load(r.cq, r.p.CQ.Overflow) != 0 || load(r.sq, r.p.SQ.Dropped) != 0 {
		return nil, errors.New("io_uring lost CQE or dropped SQE; refusing unsafe continuation")
	}
	r.events = r.events[:0]
	head = load(r.cq, r.p.CQ.Head)
	tail = load(r.cq, r.p.CQ.Tail)
	mask = load(r.cq, r.p.CQ.Mask)
	for head != tail {
		// Kernel release-publishes CQ tail. Copy the scalar CQE before releasing head.
		c := *(*cqe)(unsafe.Pointer(&r.cq[int(r.p.CQ.CQEs+(head&mask)*16)]))
		v := Completion{c.Tag, c.Result, c.Flags}
		if v.HasBuffer() {
			id := int(v.Flags >> 16)
			if id >= len(r.owned) || r.owned[id] {
				return nil, errors.New("kernel returned an invalid/already-owned buffer")
			}
			r.owned[id] = true
			r.available--
		}
		if v.Result == -int32(unix.ENOBUFS) {
			r.stats.BufferExhaustions++
		}
		r.events = append(r.events, v)
		head++
	}
	store(r.cq, r.p.CQ.Head, head)
	return r.events, nil
}
func (r *Ring) Close() error {
	if r == nil {
		return nil
	}
	if r.closeErr != nil {
		return r.closeErr
	}
	// REGISTER_SYNC_CANCEL waits for every issued operation to retire before any
	// mapping is unmapped. An unexpected failure deliberately leaks the maps until
	// process exit: freeing kernel-owned memory would be a use-after-unmap.
	if r.fd >= 0 && r.control != nil {
		clear(r.control[320:384])
		reg := (*syncCancel)(unsafe.Pointer(&r.control[320]))
		reg.Flags = 1 | 4
		reg.Timeout.Sec = 5
		if e := r.register(24, 320, 1); e != nil && e != unix.ENOENT {
			unix.Close(r.fd)
			r.fd = -1
			r.closeErr = fmt.Errorf("sync cancel (maps retained): %w", e)
			return r.closeErr
		}
		// Synchronous cancellation can leave CQEs in the NODROP overflow backlog.
		// Flush it with GETEVENTS before releasing the ring, otherwise an accepted
		// process fd present only in that backlog would leak across a failed probe.
		r.pending = nil
		// No SQPOLL: only this locked owner can submit/consume SQ. Abandon SQEs
		// published before an interrupted/EBUSY enter as well as the Go spill queue;
		// the drain enter must not issue new work after synchronous cancellation.
		if r.sq != nil {
			store(r.sq, r.p.SQ.Tail, load(r.sq, r.p.SQ.Head))
		}
		if r.cq != nil {
			for {
				events, e := r.Wait(0)
				if e != nil {
					unix.Close(r.fd)
					r.fd = -1
					r.closeErr = fmt.Errorf("close CQ drain (maps retained): %w", e)
					return r.closeErr
				}
				for _, c := range events {
					if !r.direct && c.Tag == 1 && c.Result >= 0 {
						unix.Close(int(c.Result))
					}
				}
				if len(events) == 0 && load(r.sq, r.p.SQ.Flags)&sqOverflow == 0 {
					break
				}
			}
		}
		unix.Close(r.fd)
		r.fd = -1
	}
	for _, b := range r.sends {
		unix.Munmap(b)
	}
	for _, b := range r.sendFree {
		unix.Munmap(b)
	}
	r.sends = nil
	r.sendFree = nil
	var err error
	for _, b := range [][]byte{r.buffers, r.bufRing, r.sqes, r.sq, r.control} {
		if b != nil {
			err = errors.Join(err, unix.Munmap(b))
		}
	}
	r.buffers = nil
	r.bufRing = nil
	r.sqes = nil
	r.sq = nil
	r.cq = nil
	r.control = nil
	return err
}
