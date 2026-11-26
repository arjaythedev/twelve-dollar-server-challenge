//go:build linux && (amd64 || arm64)

package main

import (
	"sort"
	"testing"

	"lowlevel/internal/netring"
)

// TestConnectionShedIdle pins the emergency shedder's selection predicate: only
// a connection with no in-flight work may be closed immediately.
func TestConnectionShedIdle(t *testing.T) {
	if !(&connection{}).shedIdle() {
		t.Fatal("a fresh idle connection must be shed-idle")
	}
	cases := map[string]*connection{
		"writer job queued":   {pending: true},
		"response in flight":  {output: []byte("HTTP/1.1 200 OK\r\n")},
		"partial request":     {input: []byte("GET /fe")},
		"draining":            {draining: true},
		"partial send offset": {output: []byte("x"), offset: 1},
	}
	for name, c := range cases {
		if c.shedIdle() {
			t.Fatalf("%s must never be shed-idle", name)
		}
	}
}

// TestShedSweepIdleNeverPicksInFlightWork exercises the batch selection used by
// the epoll loop: in-flight connections are skipped, the budget is honoured and
// only idle connections are closed.
func TestShedSweepIdleNeverPicksInFlightWork(t *testing.T) {
	conns := map[int32]*connection{
		1: {fd: 1},                                // idle
		2: {fd: 2, pending: true},                 // writer job queued
		3: {fd: 3, output: []byte("HTTP/1.1 ")},   // response in flight
		4: {fd: 4, input: []byte("GET /health ")}, // partial request
		5: {fd: 5, draining: true},                // already closing
		6: {fd: 6},                                // idle
	}
	var closed []int32
	var noted int
	closer := func(c *connection) {
		closed = append(closed, c.fd)
		delete(conns, c.fd)
	}
	n := shedSweepIdle(conns, memGuardShedBatch, closer, func() { noted++ })
	if n != 2 || len(closed) != 2 || noted != 2 {
		t.Fatalf("closed=%v n=%d noted=%d, want two idle closes", closed, n, noted)
	}
	sort.Slice(closed, func(i, j int) bool { return closed[i] < closed[j] })
	if closed[0] != 1 || closed[1] != 6 {
		t.Fatalf("closed %v, want only the idle connections 1 and 6", closed)
	}
	for _, fd := range []int32{2, 3, 4, 5} {
		if conns[fd] == nil {
			t.Fatalf("connection %d with in-flight work was closed", fd)
		}
	}
	// The budget bounds one sweep.
	conns = map[int32]*connection{1: {fd: 1}, 2: {fd: 2}, 3: {fd: 3}}
	if n = shedSweepIdle(conns, 1, func(c *connection) { delete(conns, c.fd) }, func() {}); n != 1 {
		t.Fatalf("budget 1 closed %d connections", n)
	}
}

// TestUringConnShedIdle is the io_uring analogue: a live send or a held receive
// buffer also counts as in-flight work.
func TestUringConnShedIdle(t *testing.T) {
	s := &uringConn{c: &connection{}}
	if !s.shedIdle() {
		t.Fatal("a fresh idle connection must be shed-idle")
	}
	s.sendLive = true
	if s.shedIdle() {
		t.Fatal("a live send must never be shed-idle")
	}
	s.sendLive = false
	s.pending = []netring.Completion{{}}
	if s.shedIdle() {
		t.Fatal("a held receive buffer must never be shed-idle")
	}
	s.pending = nil
	s.closing = true
	if s.shedIdle() {
		t.Fatal("a closing connection must never be shed-idle")
	}
}
