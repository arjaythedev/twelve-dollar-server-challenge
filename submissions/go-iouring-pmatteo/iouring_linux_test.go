package main

// These tests exist because nothing else checks the kernel ABI. A field at the wrong offset
// compiles fine and then silently corrupts a submission, so the layout assertions below are the
// only thing standing between a typo and a day of debugging.
//
// They need a real io_uring, so they only run on Linux with seccomp allowing io_uring_setup:
//   ./run.sh go test -v -run 'TestABI|TestRing' .

import (
	"errors"
	"syscall"
	"testing"
	"unsafe"
)

// TestABISizes pins every struct against include/uapi/linux/io_uring.h.
func TestABISizes(t *testing.T) {
	for _, c := range []struct {
		name string
		got  uintptr
		want uintptr
	}{
		{"io_uring_sqe", unsafe.Sizeof(sqe{}), 64},
		{"io_uring_cqe", unsafe.Sizeof(cqe{}), 16},
		{"io_sqring_offsets", unsafe.Sizeof(sqRingOffsets{}), 40},
		{"io_cqring_offsets", unsafe.Sizeof(cqRingOffsets{}), 40},
		{"io_uring_params", unsafe.Sizeof(ringParams{}), 120},
	} {
		if c.got != c.want {
			t.Errorf("sizeof(%s) = %d, want %d", c.name, c.got, c.want)
		}
	}
}

// TestABIOffsets pins the SQE field offsets. These are the ones a union makes easy to get wrong.
func TestABIOffsets(t *testing.T) {
	var s sqe
	base := uintptr(unsafe.Pointer(&s))
	for _, c := range []struct {
		name string
		addr uintptr
		want uintptr
	}{
		{"opcode", uintptr(unsafe.Pointer(&s.opcode)), 0},
		{"flags", uintptr(unsafe.Pointer(&s.flags)), 1},
		{"ioprio", uintptr(unsafe.Pointer(&s.ioprio)), 2},
		{"fd", uintptr(unsafe.Pointer(&s.fd)), 4},
		{"off", uintptr(unsafe.Pointer(&s.off)), 8},
		{"addr", uintptr(unsafe.Pointer(&s.addr)), 16},
		{"len", uintptr(unsafe.Pointer(&s.length)), 24},
		{"opFlags", uintptr(unsafe.Pointer(&s.opFlags)), 28},
		{"user_data", uintptr(unsafe.Pointer(&s.userData)), 32},
		{"buf_index", uintptr(unsafe.Pointer(&s.bufIndex)), 40},
		{"personality", uintptr(unsafe.Pointer(&s.personality)), 42},
		{"splice_fd_in", uintptr(unsafe.Pointer(&s.spliceFDIn)), 44},
		{"addr3", uintptr(unsafe.Pointer(&s.addr3)), 48},
	} {
		if got := c.addr - base; got != c.want {
			t.Errorf("offsetof(io_uring_sqe.%s) = %d, want %d", c.name, got, c.want)
		}
	}

	var c cqe
	cbase := uintptr(unsafe.Pointer(&c))
	if got := uintptr(unsafe.Pointer(&c.res)) - cbase; got != 8 {
		t.Errorf("offsetof(io_uring_cqe.res) = %d, want 8", got)
	}
	if got := uintptr(unsafe.Pointer(&c.flags)) - cbase; got != 12 {
		t.Errorf("offsetof(io_uring_cqe.flags) = %d, want 12", got)
	}
}

// newTestRing sets up a ring or skips the test when io_uring is unavailable, so the suite stays
// runnable on a host without it rather than failing for the wrong reason.
func newTestRing(t *testing.T, sq, cq uint32) *ring {
	t.Helper()
	r, err := newRing(sq, cq, 0)
	if err != nil {
		if errors.Is(err, syscall.ENOSYS) || errors.Is(err, syscall.EPERM) {
			t.Skipf("io_uring unavailable: %v (Docker needs --security-opt seccomp=unconfined)", err)
		}
		t.Fatalf("newRing: %v", err)
	}
	t.Cleanup(func() { r.close() })
	return r
}

// TestRingSetup checks that the kernel agreed to what we asked for and that the mapped ring
// metadata is self-consistent. A wrong offset usually shows up here as a mask of 0.
func TestRingSetup(t *testing.T) {
	r := newTestRing(t, 64, 256)

	if r.sqEntries != 64 {
		t.Errorf("sq entries = %d, want 64", r.sqEntries)
	}
	if r.cqEntries != 256 {
		t.Errorf("cq entries = %d, want 256 (IORING_SETUP_CQSIZE ignored?)", r.cqEntries)
	}
	if r.sqMask != r.sqEntries-1 {
		t.Errorf("sq mask = %d, want %d", r.sqMask, r.sqEntries-1)
	}
	if r.cqMask != r.cqEntries-1 {
		t.Errorf("cq mask = %d, want %d", r.cqMask, r.cqEntries-1)
	}
	if got := len(r.sqes); got != 64 {
		t.Errorf("len(sqes) = %d, want 64", got)
	}
	if got := len(r.cqes); got != 256 {
		t.Errorf("len(cqes) = %d, want 256", got)
	}
	if *r.sqHead != 0 || *r.sqTail != 0 || *r.cqHead != 0 || *r.cqTail != 0 {
		t.Errorf("fresh ring is not at zero: sq %d/%d cq %d/%d",
			*r.sqHead, *r.sqTail, *r.cqHead, *r.cqTail)
	}

	// featFastPoll is what makes a multishot receive cheap rather than a thread-pool offload, and
	// featNoDrop is what lets the loop treat the CQ as lossless. Both are old enough that their
	// absence means the kernel is too old for this design.
	for _, f := range []struct {
		name string
		bit  uint32
	}{
		{"IORING_FEAT_SINGLE_MMAP", featSingleMMap},
		{"IORING_FEAT_NODROP", featNoDrop},
		{"IORING_FEAT_FAST_POLL", featFastPoll},
	} {
		if r.features&f.bit == 0 {
			t.Errorf("kernel lacks %s (features=%#x)", f.name, r.features)
		}
	}
	t.Logf("features=%#x sq=%d cq=%d", r.features, r.sqEntries, r.cqEntries)
}

// TestRingNop round-trips a batch of no-ops. This is the end-to-end proof that the SQ array, the
// SQE memory, the tail store and the CQE mapping all line up: user_data comes back only if every
// one of them is right.
func TestRingNop(t *testing.T) {
	r := newTestRing(t, 64, 256)

	const n = 16
	for i := 0; i < n; i++ {
		s := r.getSQE()
		if s == nil {
			t.Fatalf("getSQE returned nil at %d with %d entries free", i, r.sqEntries)
		}
		s.prepNop(uint64(0x1000 + i))
	}
	submitted, err := r.enter(n, 0)
	if err != nil {
		t.Fatalf("enter: %v", err)
	}
	if submitted != n {
		t.Fatalf("submitted %d of %d", submitted, n)
	}

	seen := make(map[uint64]int32, n)
	got := r.forEachCQE(func(c *cqe) { seen[c.userData] = c.res })
	if got != n {
		t.Fatalf("got %d completions, want %d", got, n)
	}
	for i := 0; i < n; i++ {
		want := uint64(0x1000 + i)
		res, ok := seen[want]
		if !ok {
			t.Errorf("no completion for user_data %#x", want)
			continue
		}
		if res != 0 {
			t.Errorf("nop %#x: res = %d, want 0", want, res)
		}
	}
	if d := r.droppedSQ(); d != 0 {
		t.Errorf("sq dropped = %d: the SQ indirection array is wrong", d)
	}
	if o := r.overflowed(); o != 0 {
		t.Errorf("cq overflow = %d", o)
	}
	if ready := r.cqReady(); ready != 0 {
		t.Errorf("cq not drained: %d ready", ready)
	}
}

// TestRingSQFull checks that a full submission queue is reported rather than silently wrapping
// and overwriting entries the kernel has not read yet.
func TestRingSQFull(t *testing.T) {
	r := newTestRing(t, 8, 64)

	for i := 0; i < 8; i++ {
		if s := r.getSQE(); s == nil {
			t.Fatalf("getSQE nil at %d, want 8 available", i)
		} else {
			s.prepNop(uint64(i))
		}
	}
	if s := r.getSQE(); s != nil {
		t.Fatal("getSQE returned an entry past the ring size: it would overwrite an unread SQE")
	}
	if _, err := r.enter(8, 0); err != nil {
		t.Fatalf("enter: %v", err)
	}
	r.forEachCQE(func(*cqe) {})
	if s := r.getSQE(); s == nil {
		t.Fatal("getSQE still nil after the kernel consumed the queue")
	}
}

// TestRingCQWrap drives more completions than the CQ holds, draining as it goes, so the head and
// tail have to wrap several times. A masking error shows up here and nowhere else.
func TestRingCQWrap(t *testing.T) {
	r := newTestRing(t, 32, 64)

	const batches = 40
	const perBatch = 32
	var total, sum int
	for b := 0; b < batches; b++ {
		for i := 0; i < perBatch; i++ {
			s := r.getSQE()
			if s == nil {
				t.Fatalf("batch %d: getSQE nil at %d", b, i)
			}
			s.prepNop(uint64(b*perBatch + i))
		}
		if _, err := r.enter(perBatch, 0); err != nil {
			t.Fatalf("batch %d: enter: %v", b, err)
		}
		total += r.forEachCQE(func(c *cqe) { sum += int(c.userData) })
	}
	if want := batches * perBatch; total != want {
		t.Errorf("got %d completions over %d batches, want %d", total, batches, want)
	}
	// Every user_data exactly once: 0+1+...+(n-1).
	if want := (batches * perBatch) * (batches*perBatch - 1) / 2; sum != want {
		t.Errorf("user_data sum = %d, want %d: completions were lost or duplicated", sum, want)
	}
	if o := r.overflowed(); o != 0 {
		t.Errorf("cq overflow = %d", o)
	}
}
