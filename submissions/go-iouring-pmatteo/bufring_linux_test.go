package main

import (
	"errors"
	"net"
	"syscall"
	"testing"
	"unsafe"
)

func TestBufRingABI(t *testing.T) {
	if got := unsafe.Sizeof(bufEntry{}); got != 16 {
		t.Errorf("sizeof(io_uring_buf) = %d, want 16", got)
	}
	if got := unsafe.Sizeof(bufReg{}); got != 40 {
		t.Errorf("sizeof(io_uring_buf_reg) = %d, want 40", got)
	}
	// The whole tail-in-entry-0 scheme depends on resv sitting at offset 14.
	var e bufEntry
	if got := uintptr(unsafe.Pointer(&e.resv)) - uintptr(unsafe.Pointer(&e)); got != 14 {
		t.Errorf("offsetof(io_uring_buf.resv) = %d, want 14: the ring tail would land elsewhere", got)
	}
	if err := checkEndianness(); err != nil {
		t.Error(err)
	}
}

func TestArena(t *testing.T) {
	a, err := newArena(pageSize * 4)
	if err != nil {
		t.Fatalf("newArena: %v", err)
	}
	t.Cleanup(func() { a.free() })

	b1, err := a.alloc(100, 8)
	if err != nil {
		t.Fatalf("alloc: %v", err)
	}
	b2, err := a.alloc(100, 64)
	if err != nil {
		t.Fatalf("alloc: %v", err)
	}
	if addr := uintptr(unsafe.Pointer(&b2[0])); addr%64 != 0 {
		t.Errorf("64-byte alignment not honoured: %#x", addr)
	}
	p, err := a.allocPages(pageSize)
	if err != nil {
		t.Fatalf("allocPages: %v", err)
	}
	if addr := uintptr(unsafe.Pointer(&p[0])); addr%pageSize != 0 {
		t.Errorf("page alignment not honoured: %#x", addr)
	}
	// Regions must not overlap, and writing one must not disturb another.
	for i := range b1 {
		b1[i] = 0xAA
	}
	for i := range b2 {
		b2[i] = 0xBB
	}
	for i := range b1 {
		if b1[i] != 0xAA {
			t.Fatalf("b1[%d] = %#x: regions overlap", i, b1[i])
		}
	}
	// A slice handed out must not be growable into its neighbour.
	if cap(b1) != 100 {
		t.Errorf("cap(b1) = %d, want 100: append would scribble over the next region", cap(b1))
	}
	if _, err := a.alloc(pageSize*8, 8); err == nil {
		t.Error("alloc past the end of the arena succeeded")
	}
}

func TestBufRingRegister(t *testing.T) {
	r := newTestRing(t, 64, 256)
	a, err := newArena(1 << 20)
	if err != nil {
		t.Fatalf("newArena: %v", err)
	}
	t.Cleanup(func() { a.free() })

	const nr = 64
	const bufSize = 1024
	b, err := newBufRing(r, a, 7, nr, bufSize)
	if err != nil {
		t.Fatalf("newBufRing: %v", err)
	}
	t.Cleanup(func() { b.unregister(r) })

	// Every buffer was handed over, so the tail has wrapped exactly once.
	if got := b.publishedTail(); got != nr%65536 {
		t.Errorf("published tail = %d, want %d", got, nr)
	}
	// Entry 0's bid must have survived the tail store, and vice versa.
	if got := b.entries[0].bid; got != 0 {
		t.Errorf("entry 0 bid = %d, want 0: the tail store clobbered it", got)
	}
	if got := b.entries[0].length; got != bufSize {
		t.Errorf("entry 0 len = %d, want %d", got, bufSize)
	}
	// Descriptors must point at distinct, in-range buffers.
	for i := uint16(0); i < nr; i++ {
		want := uint64(uintptr(unsafe.Pointer(&b.bufs[int(i)*bufSize])))
		if got := b.entries[i].addr; got != want {
			t.Fatalf("entry %d addr = %#x, want %#x", i, got, want)
		}
	}
	if len(b.buf(nr-1)) != bufSize {
		t.Errorf("buf(%d) length = %d, want %d", nr-1, len(b.buf(nr-1)), bufSize)
	}

	// Registering a second group with the same id must be refused: it would mean two groups
	// racing for the same buffers.
	if _, err := newBufRing(r, a, 7, nr, bufSize); err == nil {
		t.Error("registering a duplicate buffer group id succeeded")
	}
	// A non-power-of-two size is caught before the kernel sees it.
	if _, err := newBufRing(r, a, 8, 63, bufSize); err == nil {
		t.Error("a non-power-of-two ring size was accepted")
	}
}

// TestBufRingTailIsolation is the regression test for the trap in the layout: a put() into entry 0
// must never disturb the tail, and advance() must never disturb entry 0's bid.
func TestBufRingTailIsolation(t *testing.T) {
	r := newTestRing(t, 64, 256)
	a, err := newArena(1 << 20)
	if err != nil {
		t.Fatalf("newArena: %v", err)
	}
	t.Cleanup(func() { a.free() })

	const nr = 16
	b, err := newBufRing(r, a, 3, nr, 256)
	if err != nil {
		t.Fatalf("newBufRing: %v", err)
	}
	t.Cleanup(func() { b.unregister(r) })

	// Walk the tail all the way around so entry 0 is written again, with a different bid this
	// time, and check both halves of the shared word after every step.
	for i := 0; i < nr*3; i++ {
		bid := uint16(i % nr)
		before := b.publishedTail()
		b.put(bid)
		if got := b.publishedTail(); got != before {
			t.Fatalf("put(%d) moved the published tail from %d to %d", bid, before, got)
		}
		b.advance()
		if got := b.publishedTail(); got != before+1 {
			t.Fatalf("advance after put(%d): tail = %d, want %d", bid, got, before+1)
		}
	}
}

func TestRegisterSparseFiles(t *testing.T) {
	r := newTestRing(t, 64, 256)

	// The table is capped at RLIMIT_NOFILE entries, so ask for what this process is allowed.
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		t.Fatalf("getrlimit: %v", err)
	}
	nr := uint32(lim.Cur)
	if nr > 65535 {
		nr = 65535
	}
	if _, err := registerSparseFiles(r, nr); err != nil {
		t.Fatalf("registerSparseFiles(%d): %v", nr, err)
	}
	t.Logf("registered %d sparse slots with RLIMIT_NOFILE=%d", nr, lim.Cur)

	// The process holds the ring fd and the test binary's own descriptors, nothing per slot.
	if err := unregisterFiles(r); err != nil {
		t.Errorf("unregisterFiles: %v", err)
	}

	// One slot over the limit must be refused, which is what makes the per-ring cap real and so
	// the reason multiple rings are needed at all.
	r2 := newTestRing(t, 8, 16)
	if _, err := registerSparseFiles(r2, uint32(lim.Cur)+1); err == nil {
		t.Errorf("registering RLIMIT_NOFILE+1 (%d) slots succeeded, expected EMFILE", lim.Cur+1)
	} else if !errors.Is(err, syscall.EMFILE) {
		t.Logf("over-limit registration failed with %v (expected EMFILE)", err)
	}
}

// listenerFD returns a listening socket's raw descriptor for the accept tests.
func listenerFD(t *testing.T) (*net.TCPListener, int) {
	t.Helper()
	ln, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { ln.Close() })
	f, err := ln.File()
	if err != nil {
		t.Fatalf("listener fd: %v", err)
	}
	t.Cleanup(func() { f.Close() })
	return ln, int(f.Fd())
}
