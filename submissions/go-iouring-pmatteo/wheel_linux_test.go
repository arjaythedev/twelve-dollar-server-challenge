//go:build linux

package main

import "testing"

func newWheelEngine(n int) *engine {
	e := &engine{conns: make([]conn, n), slotsPerRing: uint32(n), shedHard: 1 << 30}
	for i := range e.wheel {
		e.wheel[i] = -1
	}
	return e
}

// TestDeadlineDependsOnState pins what the wheel is keyed on. A lingering connection is waiting
// on one round trip, an open one on a user's think time, so they fall due at different times
// from the same lastSeen.
func TestDeadlineDependsOnState(t *testing.T) {
	cn := &conn{state: connOpen, lastSeen: 100}
	if got := cn.deadline(); got != 100+idleTimeout {
		t.Errorf("open deadline = %d, want %d", got, 100+idleTimeout)
	}
	cn.state = connLinger
	if got := cn.deadline(); got != 100+lingerTimeout {
		t.Errorf("lingering deadline = %d, want %d", got, 100+lingerTimeout)
	}
}

// TestWheelLinksAndUnlinks exercises the list mechanics the sweep depends on: an entry removed
// from the head, the middle and the tail must each leave the rest of its bucket reachable. A
// mislinked bucket either loses connections, which leaks their slots, or loops.
func TestWheelLinksAndUnlinks(t *testing.T) {
	e := newWheelEngine(4)
	for i := range e.conns {
		e.conns[i].state, e.conns[i].lastSeen = connOpen, 0
		e.wheelLink(uint32(i), &e.conns[i])
	}
	b := e.conns[0].bucket

	members := func() []int32 {
		var out []int32
		seen := map[int32]bool{}
		for i := e.wheel[b]; i >= 0; i = e.conns[i].next {
			if seen[i] {
				t.Fatalf("bucket %d loops at %d", b, i)
			}
			seen[i] = true
			out = append(out, i)
		}
		return out
	}
	if got := len(members()); got != 4 {
		t.Fatalf("bucket holds %d entries, want 4", got)
	}

	// Head (linked last, so index 3), then a middle one, then the last remaining.
	for _, drop := range []uint32{3, 1, 0, 2} {
		e.wheelUnlink(drop, &e.conns[drop])
		for _, m := range members() {
			if m == int32(drop) {
				t.Fatalf("unlinked %d is still in its bucket", drop)
			}
		}
	}
	if got := members(); len(got) != 0 {
		t.Fatalf("bucket holds %v after unlinking everything", got)
	}
	if e.wheel[b] != -1 {
		t.Errorf("empty bucket head = %d, want -1", e.wheel[b])
	}
}

// TestTouchRelinksOnlyWhenTheDeadlineMoves pins the hot-path guard. lastSeen has one-second
// resolution, so a connection served many times within one second must relink once, not once
// per request.
func TestTouchRelinksOnlyWhenTheDeadlineMoves(t *testing.T) {
	e := newWheelEngine(1)
	cn := &e.conns[0]
	cn.state, cn.lastSeen = connOpen, 10
	e.now = 10
	e.wheelLink(0, cn)
	before := cn.bucket

	e.touchConn(0, cn) // same second
	if cn.bucket != before {
		t.Errorf("bucket moved on a touch within the same second")
	}

	e.now = 11
	e.touchConn(0, cn)
	if cn.bucket == before {
		t.Errorf("bucket did not move when the deadline moved to %d", cn.deadline())
	}
	if e.wheel[before] != -1 {
		t.Errorf("old bucket still holds the connection")
	}
	if e.wheel[cn.bucket] != 0 {
		t.Errorf("new bucket %d head = %d, want 0", cn.bucket, e.wheel[cn.bucket])
	}
}

// TestSweepSkipsWrappedDeadlines pins the one case a wheel gets wrong: the wheel is 256 buckets
// and the longest timeout is 180, so a bucket being swept can also hold a connection whose
// deadline is still in the future. Closing it would be closing a live connection early.
//
// The engine has no mapped ring, so a close would dereference a nil submission queue. That is
// the assertion.
func TestSweepSkipsWrappedDeadlines(t *testing.T) {
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("the sweep closed a connection whose deadline had not passed: %v", r)
		}
	}()
	e := newWheelEngine(1)
	e.now, e.wheelNow = 1000, 999
	cn := &e.conns[0]
	// Due far in the future, but hand-linked into the bucket this tick sweeps.
	cn.state, cn.lastSeen = connOpen, e.now
	e.wheelLinkAt(0, cn, uint8(e.now&(wheelSize-1)))

	e.sweepIdle()

	if !cn.alive() {
		t.Error("connection was closed despite a deadline in the future")
	}
	if e.wheelNow != e.now {
		t.Errorf("wheelNow = %d after the sweep, want %d", e.wheelNow, e.now)
	}
}
