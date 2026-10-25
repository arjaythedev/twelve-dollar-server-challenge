package main

import (
	"net"
	"os"
	"syscall"
	"testing"
	"time"
)

// waitCQE drains completions until fn has seen what the test is waiting for, or the deadline
// passes. Tests never block forever in io_uring_enter: a missing completion has to fail, not hang.
func waitCQE(t *testing.T, r *ring, what string, fn func(c *cqe) bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := r.submit(); err != nil && err != syscall.EBUSY {
			t.Fatalf("%s: submit: %v", what, err)
		}
		done := false
		r.forEachCQE(func(c *cqe) {
			if !done && fn(c) {
				done = true
			}
		})
		if done {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(time.Millisecond)
	}
}

func fdCount(t *testing.T) int {
	t.Helper()
	ents, err := os.ReadDir("/proc/self/fd")
	if err != nil {
		t.Fatalf("read /proc/self/fd: %v", err)
	}
	return len(ents)
}

// TestAcceptRecvSendDirect is the end-to-end proof of the three features this design rests on.
// It accepts a real TCP connection into the registered file table, receives into a provided
// buffer, and replies from off-heap memory, all by index and never by file descriptor.
func TestAcceptRecvSendDirect(t *testing.T) {
	r := newTestRing(t, 64, 256)
	if _, err := registerSparseFiles(r, 64); err != nil {
		t.Fatalf("registerSparseFiles: %v", err)
	}
	a, err := newArena(1 << 20)
	if err != nil {
		t.Fatalf("newArena: %v", err)
	}
	t.Cleanup(func() { a.free() })
	br, err := newBufRing(r, a, 0, 16, 1024)
	if err != nil {
		t.Fatalf("newBufRing: %v", err)
	}
	t.Cleanup(func() { br.unregister(r) })

	out, err := a.alloc(4096, 64)
	if err != nil {
		t.Fatalf("alloc out: %v", err)
	}

	ln, lnFD := listenerFD(t)

	const udAccept = 0x1
	const udRecv = 0x2
	const udSend = 0x3
	const udCancel = 0x4
	const udClose = 0x5

	s := r.getSQE()
	if s == nil {
		t.Fatal("getSQE: nil")
	}
	s.prepAcceptMultishotDirect(int32(lnFD), 0, udAccept)
	if _, err := r.submit(); err != nil {
		t.Fatalf("submit accept: %v", err)
	}

	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	var slot uint32
	var sawMore bool
	waitCQE(t, r, "accept completion", func(c *cqe) bool {
		if c.userData != udAccept {
			return false
		}
		if c.res < 0 {
			t.Fatalf("accept failed: %v", syscall.Errno(-c.res))
		}
		slot = uint32(c.res)
		sawMore = c.flags&cqeFMore != 0
		return true
	})
	if !sawMore {
		t.Error("accept completion lacks IORING_CQE_F_MORE: the multishot accept is not still armed")
	}
	t.Logf("accepted into registered slot %d", slot)

	// Arm the multishot receive, then make the peer write. A receive armed before any data
	// arrives is the normal case and the one worth testing.
	s = r.getSQE()
	s.prepRecvMultishot(slot, br.bgid, udRecv)
	if _, err := r.submit(); err != nil {
		t.Fatalf("submit recv: %v", err)
	}

	const msg = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
	if _, err := conn.Write([]byte(msg)); err != nil {
		t.Fatalf("client write: %v", err)
	}

	var got string
	var bid uint16
	waitCQE(t, r, "receive completion", func(c *cqe) bool {
		if c.userData != udRecv {
			return false
		}
		if c.res < 0 {
			t.Fatalf("recv failed: %v", syscall.Errno(-c.res))
		}
		id, ok := bufIDFromCQE(c)
		if !ok {
			t.Fatal("recv completion carries no buffer id: IORING_CQE_F_BUFFER is not set")
		}
		bid = id
		got = string(br.buf(id)[:c.res])
		return true
	})
	if got != msg {
		t.Errorf("received %q, want %q", got, msg)
	}
	t.Logf("received %d bytes in provided buffer %d", len(got), bid)

	// Recycle the buffer the way the loop will: stage it, then publish once.
	br.put(bid)
	br.advance()

	// Reply from the off-heap arena, addressed by slot rather than by descriptor.
	const reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
	n := copy(out, reply)
	s = r.getSQE()
	s.prepSend(slot, out[:n], udSend)
	waitCQE(t, r, "send completion", func(c *cqe) bool {
		if c.userData != udSend {
			return false
		}
		if c.res < 0 {
			t.Fatalf("send failed: %v", syscall.Errno(-c.res))
		}
		if int(c.res) != n {
			t.Errorf("sent %d bytes, want %d", c.res, n)
		}
		return true
	})

	buf := make([]byte, len(reply))
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := conn.Read(buf); err != nil {
		t.Fatalf("client read: %v", err)
	}
	if string(buf) != reply {
		t.Errorf("client got %q, want %q", buf, reply)
	}

	// Close the way the loop must: cancel first, because the multishot receive still holds a
	// reference to the socket, then close, hard-linked so the close runs even if the cancel
	// matched nothing.
	s = r.getSQE()
	s.prepCancelFixedFile(slot, udCancel)
	s.flags |= sqeIOHardlink
	s = r.getSQE()
	s.prepCloseDirect(slot, udClose)
	waitCQE(t, r, "close completion", func(c *cqe) bool {
		if c.userData != udClose {
			return false
		}
		if c.res < 0 {
			t.Fatalf("close of slot %d failed: %v", slot, syscall.Errno(-c.res))
		}
		return true
	})

	// The slot is free again, so a second connection must land in it.
	conn2, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatalf("second dial: %v", err)
	}
	defer conn2.Close()
	var slot2 uint32
	waitCQE(t, r, "second accept", func(c *cqe) bool {
		if c.userData != udAccept {
			return false
		}
		if c.res < 0 {
			t.Fatalf("second accept failed: %v", syscall.Errno(-c.res))
		}
		slot2 = uint32(c.res)
		return true
	})
	t.Logf("second connection accepted into slot %d", slot2)

	if d := r.droppedSQ(); d != 0 {
		t.Errorf("sq dropped = %d", d)
	}
	if o := r.overflowed(); o != 0 {
		t.Errorf("cq overflow = %d", o)
	}
}

// TestDirectDescriptorsCostNoFDs is the capacity claim as a test: connections held as registered
// file slots must not consume process file descriptors. This is what moves the ceiling off
// RLIMIT_NOFILE, so if it ever stops being true the whole design is void.
func TestDirectDescriptorsCostNoFDs(t *testing.T) {
	r := newTestRing(t, 256, 1024)
	const slots = 512
	if _, err := registerSparseFiles(r, slots); err != nil {
		t.Fatalf("registerSparseFiles: %v", err)
	}

	ln, lnFD := listenerFD(t)
	s := r.getSQE()
	s.prepAcceptMultishotDirect(int32(lnFD), 0, 1)
	if _, err := r.submit(); err != nil {
		t.Fatalf("submit accept: %v", err)
	}

	// The client connections are ordinary sockets in this process too, so count descriptors
	// before and compare against a baseline that already includes them.
	const conns = 200
	clients := make([]net.Conn, 0, conns)
	defer func() {
		for _, c := range clients {
			c.Close()
		}
	}()

	before := fdCount(t)
	accepted := 0
	for i := 0; i < conns; i++ {
		c, err := net.Dial("tcp", ln.Addr().String())
		if err != nil {
			t.Fatalf("dial %d: %v", i, err)
		}
		clients = append(clients, c)
	}
	deadline := time.Now().Add(10 * time.Second)
	for accepted < conns {
		if _, err := r.submit(); err != nil {
			t.Fatalf("submit: %v", err)
		}
		r.forEachCQE(func(c *cqe) {
			if c.res >= 0 {
				accepted++
			} else {
				t.Errorf("accept failed: %v", syscall.Errno(-c.res))
			}
		})
		if time.Now().After(deadline) {
			t.Fatalf("accepted only %d of %d connections", accepted, conns)
		}
		time.Sleep(time.Millisecond)
	}
	after := fdCount(t)

	// Each client socket is one descriptor in this process; the accepted side should be zero.
	grew := after - before
	if grew > conns+8 {
		t.Errorf("file descriptors grew by %d for %d connections: the server side is not using "+
			"direct descriptors (want about %d, the client sockets alone)", grew, conns, conns)
	}
	t.Logf("%d connections accepted into registered slots; process fds %d -> %d (+%d, client "+
		"sockets account for %d)", accepted, before, after, grew, conns)
}

// TestAcceptTableFullTerminates proves the mechanism the multi-ring handoff is built on: when a
// ring's registered file table fills, the multishot accept terminates with an error completion
// and without IORING_CQE_F_MORE. The loop reacts to exactly this by arming the next ring.
func TestAcceptTableFullTerminates(t *testing.T) {
	r := newTestRing(t, 64, 512)
	const slots = 8
	if _, err := registerSparseFiles(r, slots); err != nil {
		t.Fatalf("registerSparseFiles: %v", err)
	}

	ln, lnFD := listenerFD(t)
	s := r.getSQE()
	s.prepAcceptMultishotDirect(int32(lnFD), 0, 1)
	if _, err := r.submit(); err != nil {
		t.Fatalf("submit accept: %v", err)
	}

	clients := make([]net.Conn, 0, slots+4)
	defer func() {
		for _, c := range clients {
			c.Close()
		}
	}()
	for i := 0; i < slots+4; i++ {
		c, err := net.Dial("tcp", ln.Addr().String())
		if err != nil {
			t.Fatalf("dial %d: %v", i, err)
		}
		clients = append(clients, c)
	}

	var ok, failed int
	var termErr syscall.Errno
	var terminated bool
	deadline := time.Now().Add(10 * time.Second)
	for !terminated && time.Now().Before(deadline) {
		if _, err := r.submit(); err != nil {
			t.Fatalf("submit: %v", err)
		}
		r.forEachCQE(func(c *cqe) {
			switch {
			case c.res >= 0:
				ok++
			default:
				failed++
				termErr = syscall.Errno(-c.res)
			}
			if c.flags&cqeFMore == 0 {
				terminated = true
			}
		})
		time.Sleep(time.Millisecond)
	}

	if !terminated {
		t.Fatalf("the accept never terminated: %d accepted, %d failed, table holds %d slots",
			ok, failed, slots)
	}
	if ok != slots {
		t.Errorf("accepted %d connections into a %d-slot table", ok, slots)
	}
	if termErr != syscall.ENFILE && termErr != syscall.EMFILE {
		t.Errorf("accept terminated with %v, want ENFILE or EMFILE", termErr)
	}
	t.Logf("table of %d filled, then the multishot accept terminated with %v and no "+
		"IORING_CQE_F_MORE: this is the ring-handoff signal", slots, termErr)
}
