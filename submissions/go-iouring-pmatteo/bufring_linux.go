package main

// Provided buffer rings.
//
// A multishot receive does not own a buffer. The kernel takes one from a group the moment data
// arrives and tells us which one it took in the upper 16 bits of cqe.flags. So an idle connection
// holds no receive buffer at all, which is what keeps bytes-per-connection low enough to matter:
// at 300,000 connections, a 1 KB buffer each would be 300 MB of the 2 GB box.

import (
	"fmt"
	"sync/atomic"
	"syscall"
	"unsafe"
)

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
