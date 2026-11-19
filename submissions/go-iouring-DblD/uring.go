//go:build linux

// A minimal io_uring binding: just the parts the server uses, with no dependency.
//
// io_uring in one paragraph: two ring buffers are shared between us and the kernel. We write
// requests ("accept on this socket", "receive on that one", "send these bytes") into the
// SUBMISSION queue, and the kernel writes results into the COMPLETION queue. One system call,
// io_uring_enter, hands over everything queued and waits for results. So the cost of talking
// to the kernel is paid once per loop pass, not once per request.
//
// Everything the kernel reads or writes lives in mmap'd memory, never on the Go heap, so the
// garbage collector can neither move it nor has to scan it.
package main

import (
	"fmt"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Numbers from the kernel's include/uapi/linux/io_uring.h. All exist in Linux 6.8.
const (
	sysIoUringSetup    = 425
	sysIoUringEnter    = 426
	sysIoUringRegister = 427

	setupCQSize       = 1 << 3  // we choose the completion queue size ourselves
	setupSubmitAll    = 1 << 7  // keep submitting after one request fails
	setupSingleIssuer = 1 << 12 // promise: only one thread ever submits (lets the kernel skip locking)
	setupDeferTaskrun = 1 << 13 // kernel does completion work only when we call enter, never by interrupting us

	enterGetEvents = 1 << 0
	enterExtArg    = 1 << 3 // the last argument is a struct that can carry a timeout

	opAccept      = 13
	opAsyncCancel = 14
	opClose       = 19
	opSend        = 26
	opRecv        = 27
	opShutdown    = 34

	acceptMultishot = 1 << 0 // one accept request keeps producing connections
	recvMultishot   = 1 << 1 // one receive request keeps producing data
	sqeFixedFile    = 1 << 0 // "fd" is a slot in the ring's own file table, not a process fd
	sqeIOHardlink   = 1 << 3 // run the next entry after this one, even if this one fails
	sqeBufferSelect = 1 << 5 // "kernel, pick a buffer from my pool when data arrives"

	// Stage 4: direct descriptors. A ring can own a table of files that is separate from the
	// process's descriptor table, so sockets in it do not count against RLIMIT_NOFILE.
	registerFiles2 = 13
	rsrcSparse     = 1 << 0     // register the table empty
	fileIndexAlloc = 0xffffffff // "kernel, put the accepted socket in any free slot"

	cqeFBuffer = 1 << 0 // completion carries a buffer id in the top 16 bits of flags
	cqeFMore   = 1 << 1 // the multishot request is still armed; if absent, re-arm it

	registerPbufRing = 22

	offSQRing = 0
	offSQEs   = 0x10000000
)

// sqe is one submission queue entry (64 bytes, layout fixed by the kernel).
type sqe struct {
	opcode      uint8
	flags       uint8
	ioprio      uint16
	fd          int32
	off         uint64
	addr        uint64
	len         uint32
	opFlags     uint32
	userData    uint64
	bufGroup    uint16
	personality uint16
	fileIndex   int32
	addr3       uint64
	_           uint64
}

// cqe is one completion queue entry (16 bytes).
type cqe struct {
	userData uint64
	res      int32
	flags    uint32
}

type ringOffsets struct {
	head, tail, ringMask, ringEntries, flagsOrOverflow, droppedOrCqes, arrayOrFlags, resv1 uint32
	userAddr                                                                               uint64
}

type uringParams struct {
	sqEntries, cqEntries, flags, sqThreadCPU, sqThreadIdle, features, wqFd uint32
	resv                                                                   [3]uint32
	sqOff, cqOff                                                           ringOffsets
}

type timespec struct{ sec, nsec int64 }

// getEventsArg is io_uring_enter's extended argument. Only the timeout is used.
type getEventsArg struct {
	sigmask   uint64
	sigmaskSz uint32
	_         uint32
	ts        uint64 // pointer to a timespec
}

type ring struct {
	fd int

	// Kept in the ring (on the heap, which Go never moves) because the kernel is handed
	// their addresses as plain numbers.
	arg getEventsArg
	ts  timespec

	sqHead, sqTail *uint32
	sqMask         uint32
	sqEntries      uint32
	sqes           []sqe
	tail           uint32 // our private tail: entries filled but not yet handed over
	submitted      uint32 // entries the kernel has been told about

	cqHead, cqTail *uint32
	cqMask         uint32
	cqes           []cqe
}

func mmapAnon(size int) []byte {
	b, err := syscall.Mmap(-1, 0, size, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANON|syscall.MAP_NORESERVE)
	if err != nil {
		panic(fmt.Sprintf("mmap %d bytes: %v", size, err))
	}
	return b
}

func newRing(sqEntries, cqEntries uint32) (*ring, error) {
	if unsafe.Sizeof(sqe{}) != 64 || unsafe.Sizeof(cqe{}) != 16 || unsafe.Sizeof(uringParams{}) != 120 {
		panic("io_uring struct layout is wrong")
	}
	p := uringParams{cqEntries: cqEntries, flags: setupCQSize | setupSubmitAll | setupSingleIssuer | setupDeferTaskrun}
	fd, _, errno := syscall.Syscall(sysIoUringSetup, uintptr(sqEntries), uintptr(unsafe.Pointer(&p)), 0)
	if errno != 0 {
		return nil, fmt.Errorf("io_uring_setup: %w", errno)
	}
	r := &ring{fd: int(fd), sqEntries: p.sqEntries}

	// Since Linux 5.4 one mapping holds both rings' head/tail words; the entries of the
	// completion queue are in it too. The submission entries are a second mapping.
	sqSize := int(p.sqOff.arrayOrFlags) + int(p.sqEntries)*4
	cqSize := int(p.cqOff.droppedOrCqes) + int(p.cqEntries)*16
	size := max(sqSize, cqSize)
	mem, err := syscall.Mmap(r.fd, offSQRing, size, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
	if err != nil {
		return nil, fmt.Errorf("mmap rings: %w", err)
	}
	base := unsafe.Pointer(&mem[0])
	u32 := func(off uint32) *uint32 { return (*uint32)(unsafe.Add(base, off)) }

	r.sqHead, r.sqTail, r.sqMask = u32(p.sqOff.head), u32(p.sqOff.tail), *u32(p.sqOff.ringMask)
	r.cqHead, r.cqTail, r.cqMask = u32(p.cqOff.head), u32(p.cqOff.tail), *u32(p.cqOff.ringMask)
	r.cqes = unsafe.Slice((*cqe)(unsafe.Add(base, p.cqOff.droppedOrCqes)), p.cqEntries)

	// The submission queue is indirect: an array of indexes into the entries. We never
	// reorder, so slot i always points at entry i.
	sqArray := unsafe.Slice(u32(p.sqOff.arrayOrFlags), p.sqEntries)
	for i := range sqArray {
		sqArray[i] = uint32(i)
	}
	sqeMem, err := syscall.Mmap(r.fd, offSQEs, int(p.sqEntries)*64, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
	if err != nil {
		return nil, fmt.Errorf("mmap sqes: %w", err)
	}
	r.sqes = unsafe.Slice((*sqe)(unsafe.Pointer(&sqeMem[0])), p.sqEntries)
	return r, nil
}

// next returns a zeroed submission entry to fill in. If the queue is full it hands the
// pending entries to the kernel first.
func (r *ring) next() *sqe {
	for i := 0; r.tail-atomic.LoadUint32(r.sqHead) == r.sqEntries; i++ {
		if i == 100 {
			panic("io_uring submission queue stays full")
		}
		r.poll()
	}
	e := &r.sqes[r.tail&r.sqMask]
	*e = sqe{}
	r.tail++
	return e
}

// reserve makes sure n entries fit without a flush in between, so that a linked pair is
// never split across two submissions.
func (r *ring) reserve(n uint32) {
	if r.sqEntries-(r.tail-atomic.LoadUint32(r.sqHead)) < n {
		r.poll()
	}
}

func (r *ring) pending() uint32 { return r.tail - r.submitted }

// enter is the system call: submit everything queued and, if flags ask for it, collect
// completions, waiting for up to minComplete of them but no longer than timeout.
// It is a RAW system call: Go's scheduler is not told we may block. There is only this one
// goroutine doing work, and telling the scheduler costs time on every call.
func (r *ring) enter(minComplete uint32, flags uintptr, timeout time.Duration, wait bool) {
	atomic.StoreUint32(r.sqTail, r.tail)
	toSubmit := r.tail - r.submitted
	var arg, argSize uintptr
	if wait {
		r.ts = timespec{sec: int64(timeout / time.Second), nsec: int64(timeout % time.Second)}
		r.arg = getEventsArg{ts: uint64(uintptr(unsafe.Pointer(&r.ts)))}
		arg, argSize, flags = uintptr(unsafe.Pointer(&r.arg)), unsafe.Sizeof(r.arg), flags|enterExtArg
	}
	for {
		n, _, errno := syscall.RawSyscall6(sysIoUringEnter, uintptr(r.fd), uintptr(toSubmit), uintptr(minComplete), flags, arg, argSize)
		switch errno {
		case 0:
			r.submitted += uint32(n)
			return
		case syscall.EINTR:
			continue
		case syscall.ETIME:
			// The wait timed out. Everything queued was still submitted.
			r.submitted += toSubmit
			return
		case syscall.EBUSY, syscall.EAGAIN:
			// Completion queue is backed up: the caller drains it and comes back.
			return
		default:
			panic(fmt.Sprintf("io_uring_enter: %v", errno))
		}
	}
}

// wait blocks until minComplete completions are ready or the window has passed.
func (r *ring) wait(minComplete uint32, window time.Duration) {
	r.enter(minComplete, enterGetEvents, window, true)
}

// poll collects whatever is ready without blocking. It asks for "as many as possible with
// a zero timeout" rather than "zero": on Linux 6.8 a ring in deferred-work mode runs only
// about 20 completions when asked for zero, which starves every ring but the one we wait on.
// (Found the hard way by go-iouring-trungdlp (#343) and go-iouring-davidefrageri (#354); see
// their READMEs. This call and the buffer-ring code below follow #354's uring.go.)
func (r *ring) poll() { r.enter(uint32(len(r.cqes)), enterGetEvents, 0, true) }

// submit hands over queued entries without collecting anything.
func (r *ring) submit() {
	if r.pending() > 0 {
		r.enter(0, 0, 0, false)
	}
}

// registerFileTable gives the ring an empty table of n direct-descriptor slots. The kernel
// refuses a table larger than RLIMIT_NOFILE, which is why one ring is not enough.
func (r *ring) registerFileTable(n uint32) error {
	reg := struct {
		nr, flags         uint32
		resv2, data, tags uint64
	}{nr: n, flags: rsrcSparse}
	if _, _, errno := syscall.Syscall6(sysIoUringRegister, uintptr(r.fd), registerFiles2, uintptr(unsafe.Pointer(&reg)), unsafe.Sizeof(reg), 0, 0); errno != 0 {
		return fmt.Errorf("register file table (%d slots): %w", n, errno)
	}
	return nil
}

// drain calls fn for every completion that is ready, then tells the kernel they are consumed.
func (r *ring) drain(fn func(c *cqe)) int {
	head := *r.cqHead
	tail := atomic.LoadUint32(r.cqTail)
	n := int(tail - head)
	for ; head != tail; head++ {
		fn(&r.cqes[head&r.cqMask])
	}
	atomic.StoreUint32(r.cqHead, head)
	return n
}

// peek calls fn for every completion that is waiting, without taking any off the queue.
func (r *ring) peek(fn func(c *cqe)) {
	for head, tail := *r.cqHead, atomic.LoadUint32(r.cqTail); head != tail; head++ {
		fn(&r.cqes[head&r.cqMask])
	}
}

// bufPool is a "provided buffer ring": a pool of receive buffers we lend to the kernel.
// A connection that is idle holds none of them. When data arrives the kernel picks one,
// tells us which, and we hand it back as soon as we have parsed the request. That is how
// 300,000 mostly-idle connections fit in memory: a few hundred buffers serve all of them.
type bufPool struct {
	mem   []byte // count * size bytes of buffers
	ring  []byte // count * 16 bytes: the list of buffers currently lent to the kernel
	size  int
	mask  uint16
	tail  uint16
	group uint16
}

type bufReg struct {
	ringAddr    uint64
	ringEntries uint32
	bgid        uint16
	flags       uint16
	resv        [3]uint64
}

func (r *ring) newBufPool(group uint16, count, size int) (*bufPool, error) {
	if count&(count-1) != 0 {
		panic("buffer count must be a power of two")
	}
	p := &bufPool{mem: mmapAnon(count * size), ring: mmapAnon(count * 16), size: size, mask: uint16(count - 1), group: group}
	reg := bufReg{ringAddr: uint64(uintptr(unsafe.Pointer(&p.ring[0]))), ringEntries: uint32(count), bgid: group}
	_, _, errno := syscall.Syscall6(sysIoUringRegister, uintptr(r.fd), registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0)
	if errno == syscall.EINVAL {
		// Ubuntu's 6.8.0-136 and later kernels wrongly reject an all-zero reserved field
		// (Launchpad bug 2162843). go-iouring-davidefrageri (#354) retries like this; so do we.
		reg.resv[0] = 1
		_, _, errno = syscall.Syscall6(sysIoUringRegister, uintptr(r.fd), registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0)
	}
	if errno != 0 {
		return nil, fmt.Errorf("register buffer ring: %w", errno)
	}
	for i := 0; i < count; i++ {
		p.give(uint16(i))
	}
	return p, nil
}

func (p *bufPool) data(id uint16, n int) []byte {
	off := int(id) * p.size
	return p.mem[off : off+n]
}

// give returns buffer id to the kernel. Each ring slot is {addr u64, len u32, id u16, _ u16};
// the shared tail counter lives in the last two bytes of slot 0.
func (p *bufPool) give(id uint16) {
	slot := unsafe.Pointer(&p.ring[int(p.tail&p.mask)*16])
	*(*uint64)(slot) = uint64(uintptr(unsafe.Pointer(&p.mem[int(id)*p.size])))
	*(*uint32)(unsafe.Add(slot, 8)) = uint32(p.size)
	*(*uint16)(unsafe.Add(slot, 12)) = id
	p.tail++
	*(*uint16)(unsafe.Pointer(&p.ring[14])) = p.tail
}
