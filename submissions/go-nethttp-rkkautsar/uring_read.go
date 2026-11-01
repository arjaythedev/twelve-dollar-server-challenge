//go:build linux

package main

import (
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	ioUringRegister = 427
	rawOpRecv       = 27
	iosqeBufSel     = 1 << 5
	recvMultishot   = 1 << 1
	cqeBuffer       = 1 << 0
	cqeMore         = 1 << 1

	eventfdSyscall     = 290
	registerEventfd    = 4
	registerPbufRing   = 22
	ioringOffPbufRing  = 0x80000000
	ioringOffPbufShift = 16

	readSqEntries = 4096
	readCqEntries = 8192
	readBufCount  = 8192
	readBufLen    = 2048
	readBufGroup  = 0
)

type readBuf struct {
	Addr uint64
	Len  uint32
	BID  uint16
	Resv uint16
}

type readBufReg struct {
	RingAddr uint64
	RingEnts uint32
	BGID     uint16
	Flags    uint16
	MinLeft  uint32
	Reserved [5]uint32
}

type readBufRing struct {
	mem     []byte
	entries uint32
	mask    uint32
	tail    uint32
}

func rawEventfd() (int, error) {
	fd, _, errno := syscall.Syscall(eventfdSyscall, 0, uintptr(0x800|0x80000), 0)
	if errno != 0 {
		return -1, errno
	}
	return int(fd), nil
}

func newReadBufRing(ring *rawRing, mem []byte, bufs []byte) (*readBufRing, error) {
	reg := readBufReg{
		RingAddr: uint64(uintptr(unsafe.Pointer(&mem[0]))),
		RingEnts: readBufCount,
		BGID:     readBufGroup,
	}
	if _, _, errno := syscall.Syscall6(ioUringRegister, uintptr(ring.fd), registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0); errno != 0 {
		return nil, errno
	}
	for i := 0; i < readBufCount; i++ {
		b := (*readBuf)(unsafe.Pointer(&mem[i*16]))
		b.Addr = uint64(uintptr(unsafe.Pointer(&bufs[i*readBufLen])))
		b.Len = readBufLen
		b.BID = uint16(i)
	}
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&mem[12])), uint32(readBufCount)<<16)
	return &readBufRing{
		mem:     mem,
		entries: readBufCount,
		mask:    readBufCount - 1,
		tail:    readBufCount,
	}, nil
}

func (b *readBufRing) put(base []byte, id int) {
	slot := b.tail & b.mask
	e := (*readBuf)(unsafe.Pointer(&b.mem[int(slot)*16]))
	e.Addr = uint64(uintptr(unsafe.Pointer(&base[id*readBufLen])))
	e.Len = readBufLen
	e.BID = uint16(id)
	e.Resv = 0
	b.tail = (b.tail + 1) & 0xffff
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&b.mem[12])), b.tail<<16)
}
