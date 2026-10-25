package main

// The io_uring layer, on raw syscalls.
//
// There is no liburing here and no cgo: the I/O path must not pin a C dependency or cross the cgo
// boundary per request. The cost is that nothing is checking the kernel ABI for us, so every
// struct below is laid out by hand against include/uapi/linux/io_uring.h and its size is asserted
// in iouring_linux_test.go. A field at the wrong offset does not fail to compile; it corrupts a
// submission queue entry, so the size and offset assertions are load-bearing.

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
