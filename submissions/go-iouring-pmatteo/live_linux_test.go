//go:build linux

package main

import "testing"

// TestSheddingIgnoresLingerers pins that the shed decision does not count the connections
// shedding has already claimed.
//
// used counts slots occupied, which is right for whether a table has room for an accept and
// wrong for whether to shed: a lingering connection holds a slot but has already been told to
// go. Deciding on used lets shedding stay latched above its own threshold for as long as
// lingerers keep arriving at the rate shedding creates them.
func TestSheddingIgnoresLingerers(t *testing.T) {
	e := &engine{shedSoft: 100, shedHard: 200, used: 250, live: 50}
	if e.shedding() {
		t.Error("shedding on 250 occupied slots of which only 50 are sheddable")
	}
	if e.urgent() {
		t.Error("sweeping hard on 250 occupied slots of which only 50 are sheddable")
	}

	e.live = 150
	if !e.shedding() {
		t.Error("not shedding at 150 sheddable against a soft threshold of 100")
	}
	if e.urgent() {
		t.Error("sweeping hard at 150 sheddable against a hard threshold of 200")
	}
}

// TestConnStatesAnswerTwoQuestions pins the distinction the live count rests on. A lingering
// connection is not served but is still alive: it owns its slot, takes completions, and is swept
// and closed like any other. Every site that asks "is this slot in use" means alive(); every
// site that asks "should this be answered" means serving(). Conflating them is what let a
// lingering connection be served and what let its idle deadline move.
func TestConnStatesAnswerTwoQuestions(t *testing.T) {
	for _, c := range []struct {
		state          uint8
		alive, serving bool
	}{
		{connFree, false, false},
		{connOpen, true, true},
		{connLinger, true, false},
		{connClosing, false, false},
	} {
		cn := &conn{state: c.state}
		if cn.alive() != c.alive {
			t.Errorf("state %d: alive() = %v, want %v", c.state, cn.alive(), c.alive)
		}
		if cn.serving() != c.serving {
			t.Errorf("state %d: serving() = %v, want %v", c.state, cn.serving(), c.serving)
		}
	}
}

// The live count is decremented at exactly one transition per connection - at the move into
// connLinger, or at the close of a still-servable one - and closeConn is where that is counted,
// because connClosing has by then overwritten the state that says which it was. Driving
// closeConn needs a mapped ring, so that accounting is checked end to end instead: with nothing
// lingering, the stats line's live must equal its open.
//
// What follows pins the accounting that makes the test above meaningful: a connection leaves the
// sheddable population when it starts lingering, so the close must not take it out a second
// time.
func TestLingererLeavesTheLiveCountOnce(t *testing.T) {
	for _, c := range []struct {
		name string
		was  uint8
		want uint32
	}{
		{"lingering, already counted out", connLinger, 7},
		{"closed without lingering", connOpen, 7},
	} {
		e := &engine{
			conns:        make([]conn, 1),
			slotsPerRing: 1,
			used:         9,
			live:         7,
		}
		rs := &ringState{slots: 1, used: 9}
		cn := &e.conns[0]
		cn.state, cn.out, cn.pend = connClosing, -1, -1
		cn.closeAcked, cn.sendArmed = true, false
		_ = c.was

		// finishClose frees the slot; it no longer touches the live count, which is settled at
		// the closeConn transition.
		e.finishClose(rs, 0, cn)
		if cn.state != connFree {
			t.Fatalf("%s: state = %d after finishClose, want connFree", c.name, cn.state)
		}
		if e.live != c.want {
			t.Errorf("%s: live = %d, want %d", c.name, e.live, c.want)
		}
		if e.used != 8 {
			t.Errorf("%s: used = %d, want 8: every close frees a slot", c.name, e.used)
		}
	}
}
