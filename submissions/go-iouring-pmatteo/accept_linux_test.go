//go:build linux

package main

import "testing"

// TestArmAcceptIsIdempotent pins the invariant that only one multishot accept is ever live.
//
// The bug this replaces: handoffAccept arms the next ring and then cancels this one, so the
// cancel's completion arrives with accept already armed elsewhere. onAccept's ECANCELED arm
// fell through to arming again, which put a second multishot accept on the listening socket.
// Nothing could then move that one, because the handoff test in openConn only fires for the
// ring in e.accepting, so it ran until its own table filled and reset the client whose socket
// the kernel could not place - and the ENFILE path armed another. Every handoff bred the next:
// across three 5-minute holds, 95% to 97% of handoffs were forced by ENFILE rather than planned,
// and each one cost a client a reset.
func TestArmAcceptIsIdempotent(t *testing.T) {
	// The ring is not mapped, so any attempt to submit dereferences a nil submission queue.
	// That is the assertion: with an accept already armed, arming must not reach the ring.
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("armAccept reached the ring while an accept was already armed: %v", r)
		}
	}()
	e := &engine{
		rings:        []*ringState{{idx: 0, slots: 64}, {idx: 1, slots: 64}},
		accepting:    1,
		acceptMargin: 8,
	}
	e.armAccept()
	if e.accepting != 1 {
		t.Fatalf("accepting = %d, want 1: the armed ring was changed", e.accepting)
	}
	if e.stats.acceptStalled != 0 {
		t.Fatal("a stall was counted while an accept was armed")
	}
}

// TestHasRoomLeavesTheMargin checks the test that decides where an accept may be armed, which
// is the same test that decides when to hand off.
func TestHasRoomLeavesTheMargin(t *testing.T) {
	e := &engine{acceptMargin: 32}
	for _, c := range []struct {
		used uint32
		want bool
	}{
		{0, true}, {927, true}, {928, false}, {960, false},
	} {
		if got := e.hasRoom(&ringState{slots: 960, used: c.used}); got != c.want {
			t.Errorf("hasRoom(used=%d) = %v, want %v", c.used, got, c.want)
		}
	}
}

// TestArmAcceptSkipsRingsInsideTheMargin pins that a table within the margin of full is passed
// over rather than armed into, and that when no table has room nothing is armed at all. Arming
// into the last slots is what produces an ENFILE, which the kernel reports only after it has
// accepted the socket, so the client is reset rather than queued in the backlog.
func TestArmAcceptSkipsRingsInsideTheMargin(t *testing.T) {
	e := &engine{
		rings:        []*ringState{{idx: 0, slots: 64, used: 60}, {idx: 1, slots: 64, used: 64}},
		accepting:    -1,
		acceptMargin: 8,
	}
	e.armAccept()
	if e.accepting != -1 {
		t.Fatalf("accepting = %d, want -1: an accept was armed on a table inside the margin",
			e.accepting)
	}
}
