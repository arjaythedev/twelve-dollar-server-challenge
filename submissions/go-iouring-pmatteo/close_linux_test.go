//go:build linux

package main

import "testing"

// TestCloseWaitsForSendCompletion pins the invariant that the two-phase close exists for: a
// connection's response buffer must not go back to the pool while the kernel may still be
// reading it.
//
// The bug this replaces: closeConn released the buffer immediately and onSend dropped the
// completion for a closing connection. A send in flight meant the kernel read the buffer after
// io_uring_enter returned, by which time another connection could have taken it from the free
// pool and written its own response into the bytes still going out. Nothing failed visibly, no
// test caught it, and the only symptom would have been garbled bytes on a wire under load.
//
// Neither completion path reaches the ring, so this needs no io_uring instance.
func TestCloseWaitsForSendCompletion(t *testing.T) {
	const gen = 1
	e := &engine{
		outSize:      64,
		outBufs:      make([]byte, 64*2),
		outFree:      []int32{1, 0},
		outTop:       2,
		pendFree:     []int32{0},
		pendTop:      1,
		conns:        make([]conn, 1),
		slotsPerRing: 1,
		used:         1,
	}
	rs := &ringState{slots: 1, used: 1}

	// A connection mid-close with a send still outstanding on its response buffer.
	cn := &e.conns[0]
	cn.state, cn.gen, cn.slot = connClosing, gen, 0
	cn.out, cn.outStart, cn.outEnd = e.takeOut(), 0, 16
	cn.sendArmed, cn.pend = true, -1
	heldTop := e.outTop

	// The close completes first. The buffer must stay out of the pool and the slot must stay
	// occupied: freeing the slot here would let the next accept reuse it and overwrite the
	// buffer under the kernel.
	e.onClose(rs, &cqe{userData: ud(kindClose, 0, gen)})
	if cn.state != connClosing {
		t.Fatalf("state = %d after close completion, want connClosing: the connection was "+
			"freed while a send was still in flight", cn.state)
	}
	if e.outTop != heldTop {
		t.Fatalf("outTop = %d, want %d: the response buffer went back to the pool while the "+
			"kernel could still read it", e.outTop, heldTop)
	}
	if !cn.closeAcked {
		t.Fatal("closeAcked not recorded, so the send completion has nothing to finish")
	}

	// Now the send reports back. That is what releases the buffer and frees the slot.
	e.onSend(rs, &cqe{userData: ud(kindSend, 0, gen), res: 16})
	if cn.state != connFree {
		t.Fatalf("state = %d after the send completed, want connFree", cn.state)
	}
	if cn.sendArmed {
		t.Fatal("sendArmed still set after the send completed")
	}
	if e.outTop != heldTop+1 {
		t.Fatalf("outTop = %d, want %d: the response buffer leaked", e.outTop, heldTop+1)
	}
	if cn.out != -1 {
		t.Fatalf("cn.out = %d, want -1", cn.out)
	}
	if rs.used != 0 || e.used != 0 {
		t.Fatalf("used = %d/%d, want 0/0: the slot leaked", rs.used, e.used)
	}
	if cn.gen == gen {
		t.Fatal("generation not bumped, so a late completion would be taken for a live one")
	}
}

// TestCloseReleasesBufferWhenNoSendInFlight is the other half: with nothing in flight the buffer
// goes back at once, so the deferral above is not paid for by a connection that does not need it.
func TestCloseReleasesBufferWhenNoSendInFlight(t *testing.T) {
	const gen = 3
	e := &engine{
		outSize:      64,
		outBufs:      make([]byte, 64),
		outFree:      []int32{0},
		outTop:       1,
		pendFree:     []int32{0},
		pendTop:      1,
		conns:        make([]conn, 1),
		slotsPerRing: 1,
		used:         1,
	}
	rs := &ringState{slots: 1, used: 1}
	cn := &e.conns[0]
	cn.state, cn.gen, cn.slot = connClosing, gen, 0
	cn.out, cn.pend = e.takeOut(), -1
	cn.sendArmed = false

	e.onClose(rs, &cqe{userData: ud(kindClose, 0, gen)})
	if cn.state != connFree {
		t.Fatalf("state = %d, want connFree: with no send in flight the close completes now", cn.state)
	}
	if e.outTop != 1 {
		t.Fatalf("outTop = %d, want 1: the buffer should be back in the pool", e.outTop)
	}
}
