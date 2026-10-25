//go:build linux

package main

import "testing"

// TestLingerDeadlineDoesNotMove pins the bound that lingerTimeout is supposed to provide.
//
// The bug this replaces: the receive path refreshed lastSeen before testing linger, so a client
// that was told the connection was ending and then kept sending pushed its own deadline out on
// every arrival. The 10-second bound could never fire, and the slot was held for the full
// 180-second idle timeout instead - by exactly the misbehaving client the bound exists for, and
// reachable without malice, because a 431 response takes the same path.
func TestLingerDeadlineDoesNotMove(t *testing.T) {
	cn := &conn{state: connOpen, lastSeen: 100}

	// An ordinary connection's deadline follows its traffic.
	cn.touch(140)
	if cn.lastSeen != 140 {
		t.Fatalf("lastSeen = %d, want 140: an open connection must be touched", cn.lastSeen)
	}

	// Once it is lingering, nothing the client sends may move it.
	cn.state = connLinger
	cn.touch(200)
	if cn.lastSeen != 140 {
		t.Fatalf("lastSeen = %d, want 140: a lingering connection refreshed its own deadline, "+
			"so lingerTimeout would never fire", cn.lastSeen)
	}
}

// TestLingerTimeoutIsShorterThanIdle states the relationship the sweep depends on: a lingering
// connection is waiting on one round trip, not on a user's think time.
func TestLingerTimeoutIsShorterThanIdle(t *testing.T) {
	if lingerTimeout >= idleTimeout {
		t.Fatalf("lingerTimeout = %d, idleTimeout = %d: a connection waiting for a FIN must be "+
			"reclaimed sooner than an idle user", lingerTimeout, idleTimeout)
	}
}
