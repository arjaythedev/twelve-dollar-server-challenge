package main

import (
	"bytes"
	"testing"
)

func TestCheckpointHoldPreservesPipelineAndEOF(t *testing.T) {
	s := testServer(2)
	c, other := &s.rings[0].conns[0], &s.rings[0].conns[1]
	s.holding = true
	write := []byte("POST /missing HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
	read := []byte("GET /missing HTTP/1.1\r\n\r\n")
	s.received(0, 0, c, write[:20])
	s.received(0, 0, c, write[20:])
	s.received(0, 0, c, read)
	s.received(0, 1, other, read)
	s.finish(0, 0, c)
	s.runHeld()
	if c.out != 0 || c.flags&fHeld == 0 || c.flags&fEOF == 0 || c.flags&fCloseAfter != 0 {
		t.Fatal("held pipeline executed or closed before its write")
	}
	if s.held.len() != 1 || s.ready.len() != 0 || other.out == 0 {
		t.Fatal("held connection duplicated or blocked another connection's read")
	}
	s.holding = false
	s.answers = 1
	s.runHeld()
	if bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")) != 1 || c.flags&fReady == 0 || c.flags&fCloseAfter != 0 {
		t.Fatal("release exceeded the pass budget or closed an unfinished pipeline")
	}
	s.answers = 1
	s.runReady()
	if bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")) != 2 || c.part != 0 || c.flags&fCloseAfter == 0 {
		t.Fatal("held pipeline or half-close was lost")
	}
}

func TestCheckpointHoldAfterEarlierRead(t *testing.T) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	s.holding = true
	s.received(0, 0, c, []byte("GET /missing HTTP/1.1\r\n\r\nPOST /missing HTTP/1.1\r\nContent-Length: 0\r\n\r\n"))
	if c.out == 0 || c.flags&fHeld == 0 || s.answers != answersPerPass-1 {
		t.Fatal("held POST consumed a response budget or blocked its preceding read")
	}
	s.finishTouched(s.touched[0], true)
	first := c.wire
	s.holding = false
	s.runHeld()
	if c.out == 0 || c.wire != first || bytes.Count(s.buf(c.out), []byte("HTTP/1.1 404 ")) != 1 {
		t.Fatal("release modified an in-flight response or lost the write")
	}
	s.finishTouched(s.touched[0], false)
	if !bytes.HasPrefix(s.buf(c.out), []byte("HTTP/1.1 500 ")) || c.wire != first {
		t.Fatal("rollback of a released write corrupted earlier committed output")
	}
}

func TestCheckpointHeldReferenceCannotReachReusedSlot(t *testing.T) {
	s := testServer(1)
	r, c := s.rings[0], &s.rings[0].conns[0]
	s.holding = true
	s.received(0, 0, c, []byte("POST /missing HTTP/1.1\r\nContent-Length: 0\r\n\r\n"))
	s.completeClose(r, 0, c.gen)
	*c = conn{gen: 2, flags: fOpen}
	s.holding = false
	s.runHeld()
	if c.flags != fOpen || c.part != 0 || c.out != 0 || s.held.len() != 0 {
		t.Fatal("stale held reference affected a reused connection")
	}
}
