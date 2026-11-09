package main

import (
	"bytes"
	"testing"
)

func TestPipelineBudgetAndHalfClose(t *testing.T) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	wire := []byte("GET /missing HTTP/1.1\r\n\r\n")
	s.answers = 1
	s.received(0, 0, c, bytes.Repeat(wire, 3))
	if got := bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")); got != 1 || s.answers != 0 {
		t.Fatalf("first pass: responses=%d budget=%d", got, s.answers)
	}
	if c.part == 0 || c.flags&fReady == 0 || s.ready.len() != 1 {
		t.Fatal("pipeline remainder was not queued")
	}
	s.finish(0, 0, c)
	for want := 2; want <= 3; want++ {
		s.answers = 1
		s.runReady()
		if got := bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")); got != want || s.answers != 0 {
			t.Fatalf("pass %d: responses=%d budget=%d", want, got, s.answers)
		}
		if closing := c.flags&fCloseAfter != 0; closing != (want == 3) {
			t.Fatalf("pass %d: close=%v", want, closing)
		}
	}
	if c.part != 0 || c.flags&fReady != 0 || s.ready.len() != 0 {
		t.Fatal("drained pipeline retained input or queue entry")
	}
}

func TestPartialReceiveDoesNotSpendAnswerBudget(t *testing.T) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	s.answers = 1
	s.received(0, 0, c, []byte("GET /missing HTTP/1.1\r\n"))
	if s.answers != 1 || c.out != 0 || c.flags&fReady != 0 {
		t.Fatal("incomplete request spent answer budget or became ready")
	}
	s.received(0, 0, c, []byte("\r\n"))
	if s.answers != 0 || c.part != 0 || !bytes.HasPrefix(s.buf(c.out), []byte("HTTP/1.1 404 ")) {
		t.Fatal("completed request was not answered exactly once")
	}
}

func TestStaleTouchedConnection(t *testing.T) {
	for _, committed := range []bool{false, true} {
		s := testServer(1)
		c := &s.rings[0].conns[0]
		stale := mkref(0, 0, c.gen)
		c.gen++
		c.flags |= fInBatch | fAnswered
		c.out = s.takeOut()
		s.pool[c.out-1] = append(s.buf(c.out), "new generation response"...)
		before := *c
		s.finishTouched(stale, committed)
		if *c != before || string(s.buf(before.out)) != "new generation response" || s.rings[0].u.tail != 0 {
			t.Fatalf("stale batch entry changed a reused slot (commit=%v)", committed)
		}
	}
}
