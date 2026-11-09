package main

import "testing"

// New completions must not consume the next pass before older queued work.
func TestQueuedRequestCannotBeOvertaken(t *testing.T) {
	s := testServer(2)
	wire := []byte("GET /missing HTTP/1.1\r\n\r\n")
	older, newer := &s.rings[0].conns[1], &s.rings[0].conns[0]
	s.answers = 0
	s.received(0, 1, older, wire)
	s.answers = 1
	s.received(0, 0, newer, wire)
	s.runReady()
	if older.out == 0 || newer.out != 0 || s.ready.len() != 1 {
		t.Fatal("new completion overtook an older queued request")
	}
	s.answers = 1
	s.runReady()
	if newer.out == 0 || s.ready.len() != 0 {
		t.Fatal("deferred new request was lost")
	}
}
