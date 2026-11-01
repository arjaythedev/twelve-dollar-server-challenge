// Raw io_uring ring setup and submission helpers shared by the request mux.
package main

import (
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	ioUringSetup    = 425
	ioUringEnter    = 426
	ioringOffSqRing = 0
	ioringOffCqRing = 0x8000000
	ioringOffSqes   = 0x10000000
	ioringGetEvents = 1
	uringBatch      = 64
	uringEntries    = 256
)

type rawSQE struct {
	Opcode      uint8
	Flags       uint8
	IOPrio      uint16
	FD          int32
	Off         uint64
	Addr        uint64
	Len         uint32
	RWFlags     uint32
	UserData    uint64
	BufIndex    uint16
	Personality uint16
	SpliceFDIn  int32
	Addr3       uint64
	Pad         uint64
}

type rawCQE struct {
	UserData uint64
	Res      int32
	Flags    uint32
}

type rawRing struct {
	fd      int
	sqRing  []byte
	cqRing  []byte
	sqes    []byte
	sqHead  *uint32
	sqTail  *uint32
	sqMask  *uint32
	sqArray *uint32
	cqHead  *uint32
	cqTail  *uint32
	cqMask  *uint32
	cqes    *rawCQE
	entries uint32
}

func newRawRing(entries, cqEntries uint32) (*rawRing, error) {
	var params [120]byte
	if cqEntries > 0 {
		*(*uint32)(unsafe.Pointer(&params[4])) = cqEntries
		*(*uint32)(unsafe.Pointer(&params[8])) = 1 << 3 // IORING_SETUP_CQSIZE
	}
	fd, _, errno := syscall.Syscall(ioUringSetup, uintptr(entries), uintptr(unsafe.Pointer(&params[0])), 0)
	if errno != 0 {
		return nil, errno
	}
	r := &rawRing{fd: int(fd)}
	u32 := func(off int) uint32 { return *(*uint32)(unsafe.Pointer(&params[off])) }
	r.entries = u32(0)
	sqArrayOff := u32(40 + 6*4)
	cqeCount := u32(4)
	cqHeadOff := u32(80)
	cqTailOff := u32(80 + 4)
	cqMaskOff := u32(80 + 8)
	cqCQEsOff := u32(80 + 5*4)
	sqRingLen := int(sqArrayOff + r.entries*4)
	cqRingLen := int(cqCQEsOff + cqeCount*16)
	sqeLen := int(r.entries * 64)
	sqRing, err := syscall.Mmap(int(fd), ioringOffSqRing, sqRingLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Close(int(fd))
		return nil, err
	}
	cqRing, err := syscall.Mmap(int(fd), ioringOffCqRing, cqRingLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Munmap(sqRing)
		syscall.Close(int(fd))
		return nil, err
	}
	sqes, err := syscall.Mmap(int(fd), ioringOffSqes, sqeLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Munmap(sqRing)
		syscall.Munmap(cqRing)
		syscall.Close(int(fd))
		return nil, err
	}
	r.sqRing, r.cqRing, r.sqes = sqRing, cqRing, sqes
	r.sqHead = (*uint32)(unsafe.Pointer(&sqRing[u32(40)]))
	r.sqTail = (*uint32)(unsafe.Pointer(&sqRing[u32(40+4)]))
	r.sqMask = (*uint32)(unsafe.Pointer(&sqRing[u32(40+8)]))
	r.sqArray = (*uint32)(unsafe.Pointer(&sqRing[sqArrayOff]))
	r.cqHead = (*uint32)(unsafe.Pointer(&cqRing[cqHeadOff]))
	r.cqTail = (*uint32)(unsafe.Pointer(&cqRing[cqTailOff]))
	r.cqMask = (*uint32)(unsafe.Pointer(&cqRing[cqMaskOff]))
	r.cqes = (*rawCQE)(unsafe.Pointer(&cqRing[cqCQEsOff]))
	return r, nil
}

func (r *rawRing) sqe() *rawSQE {
	tail := atomic.LoadUint32(r.sqTail)
	head := atomic.LoadUint32(r.sqHead)
	if tail-head >= r.entries {
		return nil
	}
	idx := tail & atomic.LoadUint32(r.sqMask)
	*(*uint32)(unsafe.Pointer(uintptr(unsafe.Pointer(r.sqArray)) + uintptr(idx)*4)) = idx
	sqe := (*rawSQE)(unsafe.Pointer(&r.sqes[uintptr(idx)*64]))
	atomic.StoreUint32(r.sqTail, tail+1)
	return sqe
}

func (r *rawRing) enter(toSubmit, minComplete, flags uint32) (int, error) {
	for {
		n, _, errno := syscall.Syscall6(ioUringEnter, uintptr(r.fd), uintptr(toSubmit), uintptr(minComplete), uintptr(flags), 0, 0)
		if errno == syscall.EINTR {
			continue
		}
		if errno != 0 {
			return 0, errno
		}
		return int(n), nil
	}
}

func (r *rawRing) drain(fn func(*rawCQE)) int {
	head := atomic.LoadUint32(r.cqHead)
	tail := atomic.LoadUint32(r.cqTail)
	n := 0
	for head != tail {
		cqe := (*rawCQE)(unsafe.Pointer(uintptr(unsafe.Pointer(r.cqes)) + uintptr(head&atomic.LoadUint32(r.cqMask))*unsafe.Sizeof(rawCQE{})))
		fn(cqe)
		head++
		n++
	}
	atomic.StoreUint32(r.cqHead, head)
	return n
}
