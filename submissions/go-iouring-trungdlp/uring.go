package main

// A minimal io_uring binding: setup, the shared rings, and the few operations the server uses.
// No liburing and no third-party package; everything is the kernel ABI (include/uapi/linux/io_uring.h).

import (
	"errors"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	sysIoUringSetup    = 425
	sysIoUringEnter    = 426
	sysIoUringRegister = 427

	setupCQSize       = 1 << 3
	setupSubmitAll    = 1 << 7
	setupSingleIssuer = 1 << 12
	setupDeferTaskrun = 1 << 13

	enterGetEvents = 1 << 0
	enterExtArg    = 1 << 3

	featSingleMmap = 1 << 0

	offSQRing = 0
	offCQRing = 0x8000000
	offSQEs   = 0x10000000

	opAccept   = 13
	opClose    = 19
	opSend     = 26
	opRecv     = 27
	opShutdown = 34

	sqeFixedFile    = 1 << 0
	sqeIOHardlink   = 1 << 3
	sqeBufferSelect = 1 << 5

	recvMultishot = 1 << 1

	cqeFBuffer = 1 << 0
	cqeFMore   = 1 << 1

	registerFiles2   = 13
	registerPbufRing = 22
	rsrcSparse       = 1

	fileIndexAlloc = 0xffffffff
)

type sqOffsets struct {
	head, tail, ringMask, ringEntries, flags, dropped, array, resv1 uint32
	userAddr                                                        uint64
}

type cqOffsets struct {
	head, tail, ringMask, ringEntries, overflow, cqes, flags, resv1 uint32
	userAddr                                                       uint64
}

type params struct {
	sqEntries, cqEntries, flags, sqThreadCPU, sqThreadIdle, features, wqFd uint32
	resv                                                                  [3]uint32
	sqOff                                                                 sqOffsets
	cqOff                                                                 cqOffsets
}

type sqe struct {
	opcode   uint8
	flags    uint8
	ioprio   uint16
	fd       int32
	off      uint64
	addr     uint64
	len      uint32
	opFlags  uint32
	userData uint64
	bufIndex uint16
	persona  uint16
	fileIdx  uint32
	addr3    uint64
	pad      uint64
}

type cqe struct {
	userData uint64
	res      int32
	flags    uint32
}

type getEventsArg struct {
	sigmask   uint64
	sigmaskSz uint32
	pad       uint32
	ts        uint64
}

type timespec struct{ sec, nsec int64 }

type uring struct {
	fd      int
	sqHead  *uint32
	sqTail  *uint32
	sqMask  uint32
	sqes    []sqe
	tail    uint32 // local SQ tail (published on submit)
	pending uint32 // entries pushed since the last enter

	cqHead *uint32
	cqTail *uint32
	cqMask uint32
	cqes   []cqe
}

func mmap(fd int, off int64, size int) (unsafe.Pointer, error) {
	b, err := syscall.Mmap(fd, off, size, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED|syscall.MAP_POPULATE)
	if err != nil {
		return nil, err
	}
	return unsafe.Pointer(&b[0]), nil
}

func newUring(entries, cqEntries uint32) (*uring, error) {
	var p params
	p.flags = setupCQSize | setupSubmitAll | setupSingleIssuer | setupDeferTaskrun
	p.cqEntries = cqEntries
	fd, _, e := syscall.RawSyscall(sysIoUringSetup, uintptr(entries), uintptr(unsafe.Pointer(&p)), 0)
	if e != 0 {
		return nil, e
	}
	r := &uring{fd: int(fd)}
	if p.features&featSingleMmap == 0 {
		return nil, errors.New("io_uring: kernel too old (no IORING_FEAT_SINGLE_MMAP)")
	}
	sqSize := int(p.sqOff.array + p.sqEntries*4)
	cqSize := int(p.cqOff.cqes + p.cqEntries*uint32(unsafe.Sizeof(cqe{})))
	size := max(sqSize, cqSize)
	ring, err := mmap(r.fd, offSQRing, size)
	if err != nil {
		return nil, err
	}
	sqesP, err := mmap(r.fd, offSQEs, int(p.sqEntries)*int(unsafe.Sizeof(sqe{})))
	if err != nil {
		return nil, err
	}
	at := func(off uint32) unsafe.Pointer { return unsafe.Add(ring, off) }
	r.sqHead = (*uint32)(at(p.sqOff.head))
	r.sqTail = (*uint32)(at(p.sqOff.tail))
	r.sqMask = *(*uint32)(at(p.sqOff.ringMask))
	r.sqes = unsafe.Slice((*sqe)(sqesP), p.sqEntries)
	// The index array maps ring slot i to SQE i once and for all.
	arr := unsafe.Slice((*uint32)(at(p.sqOff.array)), p.sqEntries)
	for i := range arr {
		arr[i] = uint32(i)
	}
	r.tail = *r.sqTail
	r.cqHead = (*uint32)(at(p.cqOff.head))
	r.cqTail = (*uint32)(at(p.cqOff.tail))
	r.cqMask = *(*uint32)(at(p.cqOff.ringMask))
	r.cqes = unsafe.Slice((*cqe)(at(p.cqOff.cqes)), p.cqEntries)
	return r, nil
}

func (r *uring) register(op, arg uintptr, n uintptr) error {
	_, _, e := syscall.RawSyscall6(sysIoUringRegister, uintptr(r.fd), op, arg, n, 0, 0)
	if e != 0 {
		return e
	}
	return nil
}

// registerSparse sets up a table of n direct descriptors, all empty.
func (r *uring) registerSparse(n uint32) error {
	reg := struct {
		nr, flags uint32
		resv2     uint64
		data      uint64
		tags      uint64
	}{nr: n, flags: rsrcSparse}
	return r.register(registerFiles2, uintptr(unsafe.Pointer(&reg)), unsafe.Sizeof(reg))
}

// sqeFree is how many entries can still be queued before a submit.
func (r *uring) sqeFree() uint32 {
	return uint32(len(r.sqes)) - (r.tail - atomic.LoadUint32(r.sqHead))
}

// next returns a zeroed SQE, submitting first if the queue is full.
func (r *uring) next() *sqe {
	if r.sqeFree() == 0 {
		r.flush()
	}
	e := &r.sqes[r.tail&r.sqMask]
	*e = sqe{}
	r.tail++
	r.pending++
	return e
}

// reserve makes room for n entries (a linked pair must not be split across submissions).
func (r *uring) reserve(n uint32) {
	if r.sqeFree() < n {
		r.flush()
	}
}

// flush submits everything queued and runs completions, without waiting.
func (r *uring) flush() {
	for tries := 0; ; tries++ {
		r.enter(enterGetEvents, 0, nil)
		if r.sqeFree() > 0 || tries > 100 {
			return
		}
	}
}

// enter publishes the local tail and calls io_uring_enter. With ts set it waits for min
// completions or the timeout. Every call is a raw syscall, the wait included: as an ordinary
// blocking syscall, the runtime's sysmon thread would take the P away during each 1 ms wait and
// then keep polling every 20 us. With one thread doing all the work and no garbage collection
// there is nothing for the runtime to do meanwhile.
func (r *uring) enter(flags uint32, min uint32, ts *timespec) {
	atomic.StoreUint32(r.sqTail, r.tail)
	n := r.pending
	r.pending = 0
	if ts != nil {
		a := getEventsArg{ts: uint64(uintptr(unsafe.Pointer(ts)))}
		_, _, e := syscall.RawSyscall6(sysIoUringEnter, uintptr(r.fd), uintptr(n), uintptr(min), uintptr(flags|enterExtArg),
			uintptr(unsafe.Pointer(&a)), unsafe.Sizeof(a))
		enterErr(e)
		return
	}
	_, _, e := syscall.RawSyscall6(sysIoUringEnter, uintptr(r.fd), uintptr(n), uintptr(min), uintptr(flags), 0, 0)
	enterErr(e)
}

var enterErrs int

// enterErr reports unexpected io_uring_enter errors (the first few only).
func enterErr(e syscall.Errno) {
	if e == 0 || e == syscall.ETIME || e == syscall.EINTR || e == syscall.EAGAIN || e == syscall.EBUSY {
		return
	}
	if enterErrs < 10 {
		enterErrs++
		println("io_uring_enter:", e.Error())
	}
}

func atomicLoad(p *uint32) uint32     { return atomic.LoadUint32(p) }
func atomicStore(p *uint32, v uint32) { atomic.StoreUint32(p, v) }

// ---- operations ---------------------------------------------------------------------------------

func (r *uring) accept(lfd int, ud uint64) {
	e := r.next()
	e.opcode = opAccept
	e.fd = int32(lfd)
	e.fileIdx = fileIndexAlloc
	e.userData = ud
}

func (r *uring) recvMulti(slot uint32, ud uint64) {
	e := r.next()
	e.opcode = opRecv
	e.flags = sqeFixedFile | sqeBufferSelect
	e.ioprio = recvMultishot
	e.fd = int32(slot)
	e.userData = ud
}

// recvOnce is one MSG_DONTWAIT read into a provided buffer.
func (r *uring) recvOnce(slot uint32, ud uint64) {
	e := r.next()
	e.opcode = opRecv
	e.flags = sqeFixedFile | sqeBufferSelect
	e.fd = int32(slot)
	e.opFlags = syscall.MSG_DONTWAIT
	e.userData = ud
}

func (r *uring) send(slot uint32, p unsafe.Pointer, n int, ud uint64) {
	e := r.next()
	e.opcode = opSend
	e.flags = sqeFixedFile
	e.fd = int32(slot)
	e.addr = uint64(uintptr(p))
	e.len = uint32(n)
	e.opFlags = syscall.MSG_NOSIGNAL
	e.userData = ud
}

// shutdownClose ends any receive in flight and frees the slot.
func (r *uring) shutdownClose(slot uint32, udShut, udClose uint64) {
	r.reserve(2)
	e := r.next()
	e.opcode = opShutdown
	e.flags = sqeFixedFile | sqeIOHardlink
	e.fd = int32(slot)
	e.len = syscall.SHUT_RDWR
	e.userData = udShut
	e = r.next()
	e.opcode = opClose
	e.fileIdx = slot + 1
	e.userData = udClose
}

// ---- provided buffers -----------------------------------------------------------------------------

// bufRing is a provided-buffer ring (group 0): a receive takes a buffer only when data arrives,
// so idle connections hold no receive memory.
type bufRing struct {
	ring  unsafe.Pointer // bufCount entries of {addr u64, len u32, bid u16, resv u16}; tail at offset 14
	mem   []byte
	tail  uint16
	count uint16
	size  int
}

func newBufRing(r *uring, count uint16, size int) (*bufRing, error) {
	ringMem, err := syscall.Mmap(-1, 0, int(count)*16, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS)
	if err != nil {
		return nil, err
	}
	mem, err := syscall.Mmap(-1, 0, int(count)*size, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS)
	if err != nil {
		return nil, err
	}
	b := &bufRing{ring: unsafe.Pointer(&ringMem[0]), mem: mem, count: count, size: size}
	reg := struct {
		addr    uint64
		entries uint32
		bgid    uint16
		flags   uint16
		resv    [3]uint64
	}{addr: uint64(uintptr(b.ring)), entries: uint32(count)}
	err = r.register(registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1)
	if err == syscall.EINVAL {
		// Ubuntu 6.8.0-136+ kernels invert the check on the reserved words (Launchpad #2162843):
		// they refuse all zeroes and accept a nonzero resv[0]. A correct kernel never gets here.
		reg.resv[0] = 1
		err = r.register(registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1)
	}
	if err != nil {
		return nil, err
	}
	for i := uint16(0); i < count; i++ {
		b.add(i)
	}
	b.publish()
	return b, nil
}

func (b *bufRing) add(bid uint16) {
	e := unsafe.Add(b.ring, int(b.tail&(b.count-1))*16)
	*(*uint64)(e) = uint64(uintptr(unsafe.Pointer(&b.mem[int(bid)*b.size])))
	*(*uint32)(unsafe.Add(e, 8)) = uint32(b.size)
	*(*uint16)(unsafe.Add(e, 12)) = bid
	b.tail++
}

func (b *bufRing) publish() {
	// The tail shares the first entry's resv field; the kernel reads it with acquire semantics.
	atomic.StoreUint32((*uint32)(unsafe.Add(b.ring, 12)), uint32(b.tail)<<16|uint32(*(*uint16)(unsafe.Add(b.ring, 12))))
}

func (b *bufRing) get(bid uint16, n int) []byte {
	o := int(bid) * b.size
	return b.mem[o : o+n : o+n]
}
