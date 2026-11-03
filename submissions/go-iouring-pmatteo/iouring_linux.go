package main

import (
	"fmt"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// Syscall numbers. io_uring went into the generic syscall table, so these are the same on x86-64
// and arm64, which is why the rig can exercise this code on an arm64 host.
const (
	sysIOUringSetup    = 425
	sysIOUringEnter    = 426
	sysIOUringRegister = 427
)

// mmap offsets for the three ring regions.
const (
	offSQRing = 0
	offCQRing = 0x8000000
	offSQEs   = 0x10000000
)

// io_uring_setup flags.
const (
	setupCQSize       = 1 << 3 // take cq_entries from the params
	setupClamp        = 1 << 4 // clamp entries to the kernel maximum instead of failing
	setupSubmitAll    = 1 << 7 // do not stop a submission batch at the first error
	setupCoopTaskrun  = 1 << 8
	setupTaskrunFlag  = 1 << 9
	setupSingleIssuer = 1 << 12 // only one task ever submits: lets the kernel skip locking
	setupDeferTaskrun = 1 << 13
)

// Feature flags reported in params.features.
const (
	featSingleMMap     = 1 << 0 // the SQ and CQ rings share one mapping
	featNoDrop         = 1 << 1 // completions are never dropped, they overflow instead
	featSubmitStable   = 1 << 2
	featRWCurPos       = 1 << 3
	featCurPersonality = 1 << 4
	featFastPoll       = 1 << 5 // internal poll, which is what makes multishot recv cheap
	featPollShot32Bits = 1 << 6
	featSQPollNonfixed = 1 << 7
	featExtArg         = 1 << 8
	featNativeWorkers  = 1 << 9
	featRsrcTags       = 1 << 10
	featCQESkip        = 1 << 11
	featLinkedFile     = 1 << 12
)

// io_uring_enter flags.
const (
	enterGetEvents = 1 << 0
	enterSQWakeup  = 1 << 1
	enterExtArg    = 1 << 3
)

// SQE flags.
const (
	sqeFixedFile      = 1 << 0 // fd is an index into the registered file table, not a descriptor
	sqeIODrain        = 1 << 1
	sqeIOLink         = 1 << 2 // the next SQE is only started if this one succeeds
	sqeIOHardlink     = 1 << 3 // the next SQE runs even if this one fails
	sqeAsync          = 1 << 4
	sqeBufferSelect   = 1 << 5 // take the target buffer from a provided buffer group
	sqeCQESkipSuccess = 1 << 6 // post no completion when this SQE succeeds
)

// CQE flags.
const (
	cqeFBuffer       = 1 << 0 // the upper 16 bits of cqe.flags hold the buffer id
	cqeFMore         = 1 << 1 // this is a multishot request and more completions will follow
	cqeFSockNonempty = 1 << 2
	cqeFNotif        = 1 << 3
	cqeBufferShift   = 16
)

// Opcodes. Only the ones this server submits are named.
const (
	opNop            = 0
	opReadv          = 1
	opWritev         = 2
	opPollAdd        = 6
	opPollRemove     = 7
	opSendmsg        = 9
	opRecvmsg        = 10
	opTimeout        = 11
	opAccept         = 13
	opAsyncCancel    = 14
	opLinkTimeout    = 15
	opClose          = 19
	opFilesUpdate    = 20
	opProvideBuffers = 31
	opRemoveBuffers  = 32
	opSend           = 26
	opRecv           = 27
	opShutdown       = 34
)

// sqe is struct io_uring_sqe: 64 bytes. Several fields are unions in the kernel header, so the
// names here are the use this server makes of them, with the alternates noted.
type sqe struct {
	opcode uint8
	flags  uint8
	ioprio uint16
	fd     int32  // or a registered-file index when sqeFixedFile is set
	off    uint64 // union: off, addr2, cmd_op
	addr   uint64 // union: addr, splice_off_in
	length uint32 // union: len
	// union of every per-opcode flag word: rw_flags, poll32_events, accept_flags, msg_flags,
	// cancel_flags, ...
	opFlags     uint32
	userData    uint64
	bufIndex    uint16 // union: buf_index, buf_group
	personality uint16
	// union: splice_fd_in, file_index, addr_len. For an accept with direct descriptors this is
	// file_index.
	spliceFDIn int32
	addr3      uint64
	pad2       uint64
}

// cqe is struct io_uring_cqe: 16 bytes.
type cqe struct {
	userData uint64
	res      int32
	flags    uint32
}

// sqRingOffsets is struct io_sqring_offsets: 40 bytes.
type sqRingOffsets struct {
	head        uint32
	tail        uint32
	ringMask    uint32
	ringEntries uint32
	flags       uint32
	dropped     uint32
	array       uint32
	resv1       uint32
	userAddr    uint64
}

// cqRingOffsets is struct io_cqring_offsets: 40 bytes.
type cqRingOffsets struct {
	head        uint32
	tail        uint32
	ringMask    uint32
	ringEntries uint32
	overflow    uint32
	cqes        uint32
	flags       uint32
	resv1       uint32
	userAddr    uint64
}

// ringParams is struct io_uring_params: 120 bytes.
type ringParams struct {
	sqEntries    uint32
	cqEntries    uint32
	flags        uint32
	sqThreadCPU  uint32
	sqThreadIdle uint32
	features     uint32
	wqFD         uint32
	resv         [3]uint32
	sqOff        sqRingOffsets
	cqOff        cqRingOffsets
}

// ring is one io_uring and the mapped memory behind it.
//
// The head and tail pointers point into the kernel's shared mapping, so every read of a pointer
// the kernel writes and every write of one the kernel reads goes through sync/atomic. Go's
// atomics are sequentially consistent, which is stronger than the acquire/release the ABI asks
// for, and that matters on arm64 where the ordering is not free.
type ring struct {
	fd       int
	features uint32
	params   ringParams

	ringMem []byte // the SQ/CQ mapping (one mapping when featSingleMMap)
	cqMem   []byte // the separate CQ mapping on kernels without featSingleMMap
	sqeMem  []byte

	sqHead    *uint32
	sqTail    *uint32
	sqFlags   *uint32
	sqDropped *uint32
	sqMask    uint32
	sqEntries uint32
	sqArray   []uint32
	sqes      []sqe

	cqHead     *uint32
	cqTail     *uint32
	cqOverflow *uint32
	cqFlags    *uint32
	cqMask     uint32
	cqEntries  uint32
	cqes       []cqe

	// sqeTail is our private tail. SQEs accumulate here and only become visible to the kernel
	// when flush() publishes them, so one loop iteration's whole batch goes out in a single
	// io_uring_enter.
	sqeTail uint32
	// toSubmit counts SQEs prepared since the last successful enter.
	toSubmit uint32
}

func u32At(b []byte, off uint32) *uint32 {
	return (*uint32)(unsafe.Pointer(&b[off]))
}

// newRing sets up a ring with sqEntries submission slots and cqEntries completion slots.
//
// cqEntries is requested explicitly because the default is only 2x sqEntries, and a multishot
// accept plus multishot receives can post far more completions per iteration than the server
// submits. Running out means overflow, which costs a kernel allocation per dropped completion.
func newRing(sqEntries, cqEntries uint32, extraFlags uint32) (*ring, error) {
	r := &ring{}
	r.params.flags = setupCQSize | setupClamp | extraFlags
	r.params.cqEntries = cqEntries

	fd, _, errno := syscall.Syscall6(sysIOUringSetup,
		uintptr(sqEntries), uintptr(unsafe.Pointer(&r.params)), 0, 0, 0, 0)
	if errno != 0 {
		return nil, fmt.Errorf("io_uring_setup(%d): %w", sqEntries, errno)
	}
	r.fd = int(fd)
	r.features = r.params.features

	if err := r.mapRings(); err != nil {
		syscall.Close(r.fd)
		return nil, err
	}
	return r, nil
}

func (r *ring) mapRings() error {
	sq, cq := &r.params.sqOff, &r.params.cqOff

	sqRingSize := sq.array + r.params.sqEntries*4
	cqRingSize := cq.cqes + r.params.cqEntries*uint32(unsafe.Sizeof(cqe{}))

	// With featSingleMMap the two rings live in one mapping sized to the larger of the two, and
	// the CQ offsets index into it. Without it they are two separate mappings.
	single := r.features&featSingleMMap != 0
	if single && cqRingSize > sqRingSize {
		sqRingSize = cqRingSize
	}

	var err error
	r.ringMem, err = syscall.Mmap(r.fd, offSQRing, int(sqRingSize),
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
	if err != nil {
		return fmt.Errorf("mmap sq ring (%d bytes): %w", sqRingSize, err)
	}

	cqMem := r.ringMem
	if !single {
		r.cqMem, err = syscall.Mmap(r.fd, offCQRing, int(cqRingSize),
			syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
		if err != nil {
			return fmt.Errorf("mmap cq ring (%d bytes): %w", cqRingSize, err)
		}
		cqMem = r.cqMem
	}

	sqeBytes := int(r.params.sqEntries) * int(unsafe.Sizeof(sqe{}))
	r.sqeMem, err = syscall.Mmap(r.fd, offSQEs, sqeBytes,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
	if err != nil {
		return fmt.Errorf("mmap sqes (%d bytes): %w", sqeBytes, err)
	}

	r.sqHead = u32At(r.ringMem, sq.head)
	r.sqTail = u32At(r.ringMem, sq.tail)
	r.sqFlags = u32At(r.ringMem, sq.flags)
	r.sqDropped = u32At(r.ringMem, sq.dropped)
	r.sqMask = *u32At(r.ringMem, sq.ringMask)
	r.sqEntries = *u32At(r.ringMem, sq.ringEntries)
	r.sqArray = unsafe.Slice(u32At(r.ringMem, sq.array), r.sqEntries)
	r.sqes = unsafe.Slice((*sqe)(unsafe.Pointer(&r.sqeMem[0])), r.params.sqEntries)

	r.cqHead = u32At(cqMem, cq.head)
	r.cqTail = u32At(cqMem, cq.tail)
	r.cqOverflow = u32At(cqMem, cq.overflow)
	r.cqFlags = u32At(cqMem, cq.flags)
	r.cqMask = *u32At(cqMem, cq.ringMask)
	r.cqEntries = *u32At(cqMem, cq.ringEntries)
	r.cqes = unsafe.Slice((*cqe)(unsafe.Pointer(&cqMem[cq.cqes])), r.cqEntries)

	// The SQ indirection array never changes for us: slot i always refers to SQE i. The kernel
	// allows reordering through this array; we have no use for it, so fill it once.
	for i := uint32(0); i < r.sqEntries; i++ {
		r.sqArray[i] = i
	}
	r.sqeTail = atomic.LoadUint32(r.sqTail)
	return nil
}

// getSQE reserves the next submission entry, or returns nil when the queue is full. A nil return
// is not an error: it means the caller must submit what it has and retry.
func (r *ring) getSQE() *sqe {
	head := atomic.LoadUint32(r.sqHead)
	if r.sqeTail-head >= r.sqEntries {
		return nil
	}
	s := &r.sqes[r.sqeTail&r.sqMask]
	*s = sqe{}
	r.sqeTail++
	r.toSubmit++
	return s
}

// flush publishes every prepared SQE to the kernel.
func (r *ring) flush() {
	atomic.StoreUint32(r.sqTail, r.sqeTail)
}

// enter submits the prepared batch and optionally waits for completions. It returns the number of
// SQEs the kernel consumed.
//
// EINTR and EAGAIN are the caller's to retry: EAGAIN means the completion queue is full, which is
// resolved by draining it rather than by submitting again.
func (r *ring) enter(minComplete uint32, flags uint32) (int, error) {
	r.flush()
	if minComplete > 0 {
		flags |= enterGetEvents
	}
	n, _, errno := syscall.Syscall6(sysIOUringEnter,
		uintptr(r.fd), uintptr(r.toSubmit), uintptr(minComplete), uintptr(flags), 0, 0)
	if errno != 0 {
		return 0, errno
	}
	r.toSubmit -= uint32(n)
	return int(n), nil
}

// submit sends the batch without waiting.
func (r *ring) submit() (int, error) { return r.enter(0, 0) }

// cqReady reports how many completions are queued.
func (r *ring) cqReady() uint32 {
	return atomic.LoadUint32(r.cqTail) - *r.cqHead
}

// forEachCQE calls fn for every queued completion, then advances the head once. Advancing once
// rather than per entry is one less store to shared memory per completion.
func (r *ring) forEachCQE(fn func(c *cqe)) int {
	head := *r.cqHead
	tail := atomic.LoadUint32(r.cqTail)
	n := 0
	for ; head != tail; head++ {
		fn(&r.cqes[head&r.cqMask])
		n++
	}
	if n > 0 {
		atomic.StoreUint32(r.cqHead, head)
	}
	return n
}

// overflowed reports completions the kernel could not queue. Nonzero means the CQ is too small
// for the load, which shows up as latency rather than as an error.
func (r *ring) overflowed() uint32 { return atomic.LoadUint32(r.cqOverflow) }

// droppedSQ reports submissions the kernel rejected because of an invalid index. It should always
// be zero; anything else is a bug in this file.
func (r *ring) droppedSQ() uint32 { return atomic.LoadUint32(r.sqDropped) }

func (r *ring) close() error {
	for _, m := range [][]byte{r.sqeMem, r.cqMem, r.ringMem} {
		if m != nil {
			syscall.Munmap(m)
		}
	}
	r.sqeMem, r.cqMem, r.ringMem = nil, nil, nil
	return syscall.Close(r.fd)
}

// prepNop fills in a no-op, which exists to prove the submission and completion path works
// without involving a file descriptor.
func (s *sqe) prepNop(userData uint64) {
	s.opcode = opNop
	s.fd = -1
	s.userData = userData
}

// ---- from bufring_linux.go -----------------------------------------------------------------------------

// io_uring_register opcodes.
const (
	regFiles              = 2
	regUnregisterFiles    = 3
	regEventfd            = 4
	regUnregisterEventfd  = 5
	regFilesUpdate        = 6
	regProbe              = 8
	regPbufRing           = 22
	regUnregisterPbufRing = 23
)

// bufEntry is struct io_uring_buf: 16 bytes.
//
// Never assign a whole bufEntry. The ring's tail lives in entry 0's resv field (see bufRing), so
// zeroing or wholesale-copying an entry can destroy the tail the kernel is reading. Only addr,
// length and bid are ever written.
type bufEntry struct {
	addr   uint64
	length uint32
	bid    uint16
	resv   uint16
}

// bufReg is struct io_uring_buf_reg: 40 bytes.
type bufReg struct {
	ringAddr    uint64
	ringEntries uint32
	bgid        uint16
	flags       uint16
	resv        [3]uint64
}

// bufRing is one provided buffer group: a ring of descriptors plus the memory they point at.
//
// The layout is the awkward part of the io_uring ABI. struct io_uring_buf_ring is a union of a
// header and bufs[], so the 16-byte header *is* entry 0, and the ring tail sits in the two bytes
// that entry 0 uses for resv:
//
//	offset  0..7   entry 0 addr      | header resv1
//	offset  8..11  entry 0 len       | header resv2
//	offset 12..13  entry 0 bid       | header resv3
//	offset 14..15  entry 0 resv      | header TAIL
//
// Entry 0 is a usable buffer slot, and the tail is the one field in it nobody may touch. That is
// why bufEntry is only ever written field by field.
type bufRing struct {
	ring    []byte     // the descriptor ring, page aligned, nr*16 bytes
	entries []bufEntry // a view over it, including entry 0
	bufs    []byte     // the backing memory the descriptors point into
	bufSize int
	nr      uint16
	mask    uint16
	bgid    uint16

	// tailWord is the aligned 32-bit word holding entry 0's bid and the ring tail. A 16-bit
	// atomic store does not exist in Go, and the tail is at offset 14, so the only aligned atomic
	// that reaches it covers both halves.
	tailWord *uint32
	tail     uint16 // our copy; the published tail only moves in advance()
}

// newBufRing allocates and registers a buffer group of nr buffers of bufSize bytes each. nr must
// be a power of two and at most 32768.
func newBufRing(r *ring, a *arena, bgid, nr uint16, bufSize int) (*bufRing, error) {
	if nr == 0 || nr&(nr-1) != 0 {
		return nil, fmt.Errorf("buffer ring entries must be a power of two, got %d", nr)
	}
	b := &bufRing{bufSize: bufSize, nr: nr, mask: nr - 1, bgid: bgid}

	var err error
	if b.ring, err = a.allocPages(int(nr) * int(unsafe.Sizeof(bufEntry{}))); err != nil {
		return nil, fmt.Errorf("buffer ring %d descriptors: %w", bgid, err)
	}
	if b.bufs, err = a.allocPages(int(nr) * bufSize); err != nil {
		return nil, fmt.Errorf("buffer ring %d buffers: %w", bgid, err)
	}
	b.entries = unsafe.Slice((*bufEntry)(unsafe.Pointer(&b.ring[0])), nr)
	b.tailWord = (*uint32)(unsafe.Pointer(&b.ring[12]))

	// The mapping is fresh, so the tail is already zero. Publish it anyway: this is also the
	// assertion that tailWord points where we think it does.
	atomic.StoreUint32(b.tailWord, 0)

	reg := bufReg{
		ringAddr:    uint64(uintptr(unsafe.Pointer(&b.ring[0]))),
		ringEntries: uint32(nr),
		bgid:        bgid,
	}
	if _, _, errno := syscall.Syscall6(sysIOUringRegister,
		uintptr(r.fd), regPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0); errno != 0 {
		return nil, fmt.Errorf("register buffer ring %d (%d x %d bytes): %w",
			bgid, nr, bufSize, errno)
	}

	// Hand every buffer to the kernel up front.
	for i := uint16(0); i < nr; i++ {
		b.put(i)
	}
	b.advance()
	return b, nil
}

// buf returns the memory behind buffer id bid.
func (b *bufRing) buf(bid uint16) []byte {
	off := int(bid) * b.bufSize
	return b.bufs[off : off+b.bufSize : off+b.bufSize]
}

// put stages buffer bid for return to the kernel. It becomes visible only in advance(), so a
// whole batch of recycled buffers costs one ordered store rather than one each.
func (b *bufRing) put(bid uint16) {
	e := &b.entries[b.tail&b.mask]
	// Field by field, never a struct assignment: entry 0's resv is the ring tail.
	e.addr = uint64(uintptr(unsafe.Pointer(&b.bufs[int(bid)*b.bufSize])))
	e.length = uint32(b.bufSize)
	e.bid = bid
	b.tail++
}

// advance publishes every staged buffer with a release store, so the kernel cannot observe the
// new tail before the descriptors it points at.
func (b *bufRing) advance() {
	// Read-modify-write of the word holding entry 0's bid (low half) and the tail (high half, at
	// offset 14). Both targets are little-endian; checkEndianness asserts it at startup.
	w := atomic.LoadUint32(b.tailWord)
	atomic.StoreUint32(b.tailWord, w&0x0000FFFF|uint32(b.tail)<<16)
}

// publishedTail reads the tail back out of the ring, for tests and diagnostics.
func (b *bufRing) publishedTail() uint16 {
	return uint16(atomic.LoadUint32(b.tailWord) >> 16)
}

func (b *bufRing) unregister(r *ring) error {
	reg := bufReg{bgid: b.bgid}
	if _, _, errno := syscall.Syscall6(sysIOUringRegister,
		uintptr(r.fd), regUnregisterPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0); errno != 0 {
		return errno
	}
	return nil
}

// bufIDFromCQE pulls the buffer id out of a completion. It is only meaningful when cqeFBuffer is
// set, which it is for any completion that consumed a provided buffer.
func bufIDFromCQE(c *cqe) (uint16, bool) {
	if c.flags&cqeFBuffer == 0 {
		return 0, false
	}
	return uint16(c.flags >> cqeBufferShift), true
}

// registerSparseFiles creates a registered file table of nr empty slots.
//
// This is the mechanism the whole capacity argument rests on. A connection accepted into this
// table gets an index, not a file descriptor, so the process's descriptor count stays flat no
// matter how many connections are open. The kernel caps each table at RLIMIT_NOFILE entries, but
// the cap is per table, so N rings give N times the slots.
func registerSparseFiles(r *ring, nr uint32) ([]int32, error) {
	fds := make([]int32, nr)
	for i := range fds {
		fds[i] = -1
	}
	if _, _, errno := syscall.Syscall6(sysIOUringRegister,
		uintptr(r.fd), regFiles, uintptr(unsafe.Pointer(&fds[0])), uintptr(nr), 0, 0); errno != 0 {
		return nil, fmt.Errorf("register %d sparse file slots: %w", nr, errno)
	}
	return fds, nil
}

func unregisterFiles(r *ring) error {
	if _, _, errno := syscall.Syscall6(sysIOUringRegister,
		uintptr(r.fd), regUnregisterFiles, 0, 0, 0, 0); errno != 0 {
		return errno
	}
	return nil
}

// checkEndianness guards the one place this code depends on byte order: the 32-bit atomic that
// carries the buffer ring tail in its upper half.
func checkEndianness() error {
	v := uint32(1)
	if *(*byte)(unsafe.Pointer(&v)) != 1 {
		return fmt.Errorf("big-endian host: the buffer ring tail store assumes little-endian")
	}
	return nil
}

// ---- from socket_linux.go ------------------------------------------------------------------------------

// file_index is stored as index+1, with 0 meaning "not a fixed file". IORING_FILE_INDEX_ALLOC
// asks the kernel to pick a free slot and report it in cqe.res. The kernel field is a __u32 and
// Go's is an int32, and the value is (~0U), so it is written as -1: the same 32 bits.
const fileIndexAlloc int32 = -1

// ioprio values that turn an operation multishot.
const (
	acceptMultishot = 1 << 0 // IORING_ACCEPT_MULTISHOT
	recvMultishot   = 1 << 1 // IORING_RECV_MULTISHOT
)

// IORING_OP_ASYNC_CANCEL flags.
const (
	cancelAll     = 1 << 0 // cancel every match, not just the first
	cancelFD      = 1 << 1 // match on the fd rather than on user_data
	cancelAny     = 1 << 2
	cancelFDFixed = 1 << 3 // the fd to match is a registered-table index
)

const (
	msgNoSignal = 0x4000 // MSG_NOSIGNAL: a send to a closed peer must not raise SIGPIPE
	msgWaitAll  = 0x100
)

// setTargetFixedFile points an operation's *result* at a registered file slot.
func (s *sqe) setTargetFixedFile(index uint32) {
	s.spliceFDIn = int32(index + 1)
}

// setTargetFixedFileAlloc asks the kernel to allocate any free slot.
func (s *sqe) setTargetFixedFileAlloc() {
	s.spliceFDIn = fileIndexAlloc
}

// prepAcceptMultishotDirect arms a multishot accept on a listening socket, placing each accepted
// connection into a free slot of this ring's registered file table.
//
// It stays armed and posts one completion per connection, each with cqeFMore set. cqe.res is the
// table index. When cqeFMore is absent the accept has terminated and must be re-armed, or, once
// the table is full, handed to another ring: a full table terminates the accept with ENFILE or
// EMFILE, which is exactly the signal the multi-ring handoff is driven by.
//
// No sockaddr is requested. The peer address costs a copy per connection and this server never
// looks at it.
func (s *sqe) prepAcceptMultishotDirect(listenFD int32, flags uint32, userData uint64) {
	s.opcode = opAccept
	s.fd = listenFD
	s.addr = 0 // no sockaddr out
	s.off = 0  // no addrlen out
	s.opFlags = flags
	s.ioprio = acceptMultishot
	s.userData = userData
	s.setTargetFixedFileAlloc()
}

// prepRecvMultishot arms a multishot receive on a registered connection, taking buffers from
// group bgid as data arrives.
//
// Each completion carries the buffer id in the upper 16 bits of cqe.flags and the byte count in
// cqe.res. res == 0 means the peer closed. ENOBUFS means the group ran dry, which also terminates
// the multishot, so the caller must re-arm after refilling.
func (s *sqe) prepRecvMultishot(fileIndex uint32, bgid uint16, userData uint64) {
	s.opcode = opRecv
	s.fd = int32(fileIndex)
	s.addr = 0 // the buffer comes from the group
	s.length = 0
	s.opFlags = 0
	s.ioprio = recvMultishot
	s.bufIndex = bgid
	s.userData = userData
	s.flags = sqeFixedFile | sqeBufferSelect
}

// prepSend queues a send on a registered connection. The buffer must be off-heap: the kernel
// reads it after io_uring_enter returns.
func (s *sqe) prepSend(fileIndex uint32, buf []byte, userData uint64) {
	s.opcode = opSend
	s.fd = int32(fileIndex)
	s.addr = uint64(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
	s.length = uint32(len(buf))
	s.opFlags = msgNoSignal
	s.userData = userData
	s.flags = sqeFixedFile
}

// prepCloseDirect releases a registered file slot, closing the socket in it.
func (s *sqe) prepCloseDirect(fileIndex uint32, userData uint64) {
	s.opcode = opClose
	s.fd = 0
	s.userData = userData
	s.setTargetFixedFile(fileIndex)
}

// prepCancelFixedFile cancels every pending operation on a registered connection.
//
// This is the first half of the close sequence. A pending multishot receive holds its own
// reference to the socket, so closing the slot underneath it is not enough: the operation has to
// be cancelled first. Link this to the close with sqeIOHardlink so the close runs even when the
// cancel finds nothing, which happens whenever the receive has already completed.
func (s *sqe) prepCancelFixedFile(fileIndex uint32, userData uint64) {
	s.opcode = opAsyncCancel
	s.fd = int32(fileIndex)
	s.addr = 0
	s.opFlags = cancelFD | cancelFDFixed | cancelAll
	s.userData = userData
}

// prepCancelUserData cancels the operation carrying a given user_data. It is how a multishot
// accept is moved off a ring before its registered file table fills.
func (s *sqe) prepCancelUserData(target, userData uint64) {
	s.opcode = opAsyncCancel
	s.fd = -1
	s.addr = target
	s.opFlags = 0
	s.userData = userData
}

// Poll flags, carried in sqe->len.
const (
	pollAddMulti = 1 << 0 // IORING_POLL_ADD_MULTI: stay armed after each event
	pollAddLevel = 1 << 1 // IORING_POLL_ADD_LEVEL: level triggered, not edge
)

// prepPollAdd watches a descriptor with explicit poll flags.
func (s *sqe) prepPollAdd(fd int32, events, pollFlags uint32, userData uint64) {
	s.opcode = opPollAdd
	s.fd = fd
	s.length = pollFlags
	s.opFlags = events
	s.userData = userData
}

// prepNopUserData posts a completion without doing any work, used to wake the loop.
func (s *sqe) prepNopUserData(userData uint64) { s.prepNop(userData) }
