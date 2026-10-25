//go:build linux

package main

import "testing"

// TestMemoryPressureHoldsAdmission pins which lever each pressure pulls.
//
// Slots are freed by closing; memory is not, because a shed connection is replaced by the client
// that reconnects. So memory pressure must bound admission - without this, nothing in the design
// stops the population growing while memory is short, and the server goes on accepting a new
// socket charge per arrival while cycling the connections it already has.
func TestMemoryPressureHoldsAdmission(t *testing.T) {
	// The ring is not mapped, so reaching the submission path would panic. That is the
	// assertion: under memory pressure, arming must return before it touches the ring.
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("armAccept reached the ring while memory was short: %v", r)
		}
	}()
	e := &engine{
		rings:        []*ringState{{idx: 0, slots: 64}},
		accepting:    -1,
		acceptMargin: 8,
		memHigh:      1000,
		memCrit:      2000,
		memUsed:      1500,
	}
	e.armAccept()
	if e.accepting != -1 {
		t.Fatalf("accepting = %d, want -1: an accept was armed under memory pressure",
			e.accepting)
	}
}

// TestPressureAxesAreSeparate states that neither pressure pulls the other's lever: memory does
// not tighten the idle sweep, because evicting users faster only raises the rate at which they
// reconnect, and each reconnection is a new socket.
func TestPressureAxesAreSeparate(t *testing.T) {
	// Memory critical, slots empty.
	e := &engine{shedSoft: 100, shedHard: 200, memHigh: 1000, memCrit: 2000, memUsed: 2500}
	if e.shedding() || e.urgent() {
		t.Error("memory pressure drove the slot levers")
	}
	if !e.memPressed() || !e.memCritical() {
		t.Error("memory pressure not reported at 2500 against high 1000, crit 2000")
	}

	// Slots full, memory idle. The shed decision counts the connections shedding can still
	// act on, not the slots occupied, so live is what has to be set here.
	e = &engine{shedSoft: 100, shedHard: 200, live: 250, memHigh: 1000, memCrit: 2000}
	if !e.shedding() || !e.urgent() {
		t.Error("slot pressure not reported at 250 against soft 100, hard 200")
	}
	if e.memPressed() || e.memCritical() {
		t.Error("slot pressure drove the memory levers")
	}
}
