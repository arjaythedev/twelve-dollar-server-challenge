package main

import (
	"bytes"
	"errors"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func testServer(slots int) *server {
	head := new(uint32)
	u := &uring{sqHead: head, sqMask: sqEntries - 1, sqes: make([]sqe, sqEntries)}
	r := &ring{u: u, conns: make([]conn, slots)}
	for i := range r.conns {
		r.conns[i] = conn{gen: 1, flags: fOpen}
	}
	return &server{app: newApp(nil), rings: []*ring{r}}
}

func TestBorrowedInputAndPipelining(t *testing.T) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	first := []byte("GET /missing HTTP/1.1\r\nHost:")
	s.serve(0, 0, c, first)
	clear(first) // The provided receive buffer can be returned immediately.
	if c.part == 0 || c.out != 0 {
		t.Fatal("partial input not kept independently of the provided buffer")
	}
	s.serve(0, 0, c, []byte(" test\r\n\r\nGET /also-missing HTTP/1.1\r\n\r\n"))
	if c.part != 0 || bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")) != 2 {
		t.Fatalf("fragmented pipeline lost: %q", s.buf(c.out))
	}
	if c.wire != 0 || c.flags&fSending != 0 || len(s.touched) != 1 {
		t.Fatal("response queued before commit or connection staged twice")
	}
	s.finish(0, 0, c)
	if c.flags&fCloseAfter == 0 || c.out == 0 {
		t.Fatal("peer EOF discarded owed responses")
	}
}

func TestSendBufferOwnership(t *testing.T) {
	s := testServer(1)
	r, c := s.rings[0], &s.rings[0].conns[0]
	c.wire = s.takeOut()
	s.pool[c.wire-1] = append(s.buf(c.wire), "in flight"...)
	wire := c.wire
	c.out = s.takeOut()
	s.pool[c.out-1] = append(s.buf(c.out), "next response"...)
	c.flags |= fSending
	s.complete(0, r, ud(udSend, 0, 1, 0), 2, 0)
	if c.wire != wire || c.sent != 2 || string(s.buf(wire)) != "in flight" {
		t.Fatal("partial send released or changed in-flight output")
	}
	if r.u.sqes[0].len != uint32(len("in flight")-2) {
		t.Fatal("partial send was not continued")
	}
	s.complete(0, r, ud(udSend, 0, 1, 0), int32(len("in flight")-2), 0)
	if c.wire != 0 || c.flags&fSending != 0 || string(s.buf(c.out)) != "next response" {
		t.Fatal("completed send lost the independently staged output")
	}
	if len(s.touched) != 1 {
		t.Fatal("next output not staged for a later commit")
	}
}

func TestStaleCompletions(t *testing.T) {
	s := testServer(1)
	r, c := s.rings[0], &s.rings[0].conns[0]
	entries := make([]byte, 4*16)
	r.bufs = &bufRing{ring: unsafe.Pointer(&entries[0]), mem: make([]byte, 4*bufSize), count: 4, size: bufSize}
	c.gen = 2
	c.flags |= fSending
	c.wire = s.takeOut()
	s.pool[c.wire-1] = append(s.buf(c.wire), "current generation"...)
	for _, op := range []int{udRecv, udRead, udSend, udClose} {
		flags := uint32(0)
		if op == udRecv || op == udRead {
			flags = cqeFBuffer | 1<<16
		}
		s.complete(0, r, ud(op, 0, 1, 0), 1, flags)
	}
	if c.gen != 2 || c.flags != fOpen|fSending || c.wire == 0 || len(s.touched) != 0 {
		t.Fatal("stale completion changed a reused connection slot")
	}
	if r.bufs.tail != 2 {
		t.Fatal("stale receive leaked its provided buffer")
	}
}

func TestReceiveStarvationIsBounded(t *testing.T) {
	s := testServer(2 * bufCount)
	r := s.rings[0]
	for i := range r.conns {
		s.complete(0, r, ud(udRecv, 0, 1, uint32(i)), -int32(syscall.ENOBUFS), 0)
	}
	if got := s.issueReads(readsPerPass); got != bufCount || r.reads != bufCount {
		t.Fatalf("issued %d reads, want at most %d", got, bufCount)
	}
	if s.rq.len() != bufCount {
		t.Fatal("unserved reads were lost instead of deferred")
	}
}

func TestAcceptErrors(t *testing.T) {
	for _, e := range []syscall.Errno{syscall.EMFILE, syscall.ENOMEM, syscall.ENOBUFS} {
		s := testServer(1)
		r := s.rings[0]
		s.acceptError(r, e)
		if r.u.err != nil || !r.pauseUntil.After(time.Now()) {
			t.Fatalf("resource error %v not backed off", e)
		}
	}
	s := testServer(1)
	r := s.rings[0]
	s.acceptError(r, syscall.ENFILE)
	if !r.full {
		t.Fatal("full direct table did not request another ring")
	}
	s.acceptError(r, syscall.ECONNABORTED)
	if r.u.err != nil {
		t.Fatal("aborted connection became fatal")
	}
	s.acceptError(r, syscall.EBADF)
	if !errors.Is(r.u.err, syscall.EBADF) {
		t.Fatal("invalid listener error swallowed")
	}
}

func TestEnterKeepsPublishedSubmissions(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	u, err := newUring(8, 16)
	if err != nil {
		t.Fatal(err)
	}
	defer u.close()
	for i := 0; i < 4; i++ {
		u.next().userData = uint64(i + 1) // Zero opcode is IORING_OP_NOP.
	}
	u.pending = 0 // Reproduce bookkeeping lost after a partial/interrupted enter.
	u.enter(enterGetEvents, 4, nil)
	if u.err != nil || u.pending != 0 || atomicLoad(u.cqTail)-*u.cqHead != 4 {
		t.Fatalf("published SQEs were lost: pending=%d err=%v", u.pending, u.err)
	}
}

func TestKernelABILayout(t *testing.T) {
	for _, tc := range []struct{ got, want uintptr }{
		{unsafe.Sizeof(params{}), 120}, {unsafe.Sizeof(sqe{}), 64},
		{unsafe.Sizeof(cqe{}), 16}, {unsafe.Sizeof(getEventsArg{}), 24},
		{unsafe.Offsetof(sqe{}.fileIdx), 44},
	} {
		if tc.got != tc.want {
			t.Fatalf("kernel ABI mismatch: got %d, want %d", tc.got, tc.want)
		}
	}
}

func TestLinkedCloseCannotSplitOnSubmitFailure(t *testing.T) {
	u := &uring{fd: -1, sqHead: new(uint32), sqTail: new(uint32),
		sqMask: 7, sqes: make([]sqe, 8), tail: 7}
	u.shutdownClose(0, 1, 2) // Only one slot is free; the pair needs two.
	if !errors.Is(u.err, syscall.EBADF) || u.tail != 7 || u.sqes[7].opcode != 0 {
		t.Fatal("failed submission split a linked shutdown/close pair")
	}
}

func TestRollbackConnectionReuse(t *testing.T) {
	for _, count := range []int{1, 2} {
		s := testServer(1)
		c := &s.rings[0].conns[0]
		wire := bytes.Repeat([]byte("GET /missing HTTP/1.1\r\n\r\n"), count)
		s.serve(0, 0, c, wire)
		s.finishTouched(s.touched[0], false)
		if !bytes.HasPrefix(s.buf(c.wire), []byte("HTTP/1.1 500 ")) {
			t.Fatal("rollback exposed a successful response")
		}
		if closed := c.flags&fCloseAfter != 0; closed != (count > 1) {
			t.Fatalf("responses=%d close=%v", count, closed)
		}
		if c.flags&(fInBatch|fAnswered|fMultiAnswered) != 0 {
			t.Fatal("batch flags retained after rollback")
		}
	}
}

func BenchmarkNetworkResponse(b *testing.B) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	wire := []byte("GET /missing HTTP/1.1\r\nHost: test\r\n\r\n")
	s.serve(0, 0, c, wire)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.pool[c.out-1] = s.buf(c.out)[:0]
		s.serve(0, 0, c, wire)
	}
}
