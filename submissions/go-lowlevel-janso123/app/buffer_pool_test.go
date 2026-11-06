//go:build linux

package main

import (
	"golang.org/x/sys/unix"
	"testing"
	"time"
)

func TestBufferPoolRetainsMoreThan64WithinByteBudget(t *testing.T) {
	p := &bufferPool{}
	for i := 0; i < 65; i++ {
		p.put(make([]byte, 0, 4096))
	}
	if len(p.free) != 65 || p.freeBytes != 65*4096 {
		t.Fatalf("retained %d buffers, %d bytes", len(p.free), p.freeBytes)
	}
	for i := 0; i < 65; i++ {
		_ = p.take()
	}
	if len(p.free) != 0 || p.freeBytes != 0 {
		t.Fatal("pool accounting after take")
	}
}

func TestBufferPoolByteCap(t *testing.T) {
	p := &bufferPool{}
	for i := 0; i < bufferPoolMaxBytes/4096+1; i++ {
		p.put(make([]byte, 0, 4096))
	}
	if p.freeBytes != bufferPoolMaxBytes {
		t.Fatalf("pooled %d bytes", p.freeBytes)
	}
}

func TestChunkedCheckDoesNotBuildBodyAndDecodeReusesPool(t *testing.T) {
	raw := []byte("POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n")
	before := append([]byte(nil), raw...)
	checked, n, err := parseRequestCheck(raw)
	if err != nil || n != len(raw) || checked.job.body != nil {
		t.Fatalf("check: %d %+v %+v", n, checked, err)
	}
	if string(raw) != string(before) {
		t.Fatal("check mutated input")
	}
	p := &bufferPool{}
	r, n, err := parseRequestPooled(raw, p)
	if err != nil || n != len(raw) || string(r.job.body) != "abcde" || !r.bodyPooled {
		t.Fatalf("decode: %d %+v %+v", n, r, err)
	}
	p.put(r.job.body)
	if p.freeBytes == 0 {
		t.Fatal("body not returned to pool")
	}
}

func TestExpectContinueReturnsProvisionalChunkBody(t *testing.T) {
	p := &bufferPool{write: func(*connection, []byte) (int, error) { return 0, unix.EAGAIN }}
	c := &connection{input: []byte("POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n3\r\nabc\r\n0\r\n\r\n")}
	called := false
	closed, err := c.drive(p, func(job) reply { called = true; return reply{} }, time.Now())
	if closed || err != nil || called || string(c.output) != "HTTP/1.1 100 Continue\r\n\r\n" {
		t.Fatalf("continue state: closed=%v err=%v called=%v output=%q", closed, err, called, c.output)
	}
	if p.freeBytes != 4096 {
		t.Fatalf("provisional decoded body was not returned: %d pooled bytes", p.freeBytes)
	}
}

func TestBufferPoolBlockedDropsAndDoesNotGrow(t *testing.T) {
	p := &bufferPool{}
	for i := 0; i < 8; i++ {
		p.put(make([]byte, 0, 4096))
	}
	if p.freeBytes != 8*4096 {
		t.Fatalf("setup pooled %d bytes", p.freeBytes)
	}
	p.setBlocked(true)
	if len(p.free) != 0 || p.freeBytes != 0 {
		t.Fatalf("engage must drop the pool: %d buffers, %d bytes", len(p.free), p.freeBytes)
	}
	// While blocked the pool must not grow.
	for i := 0; i < 8; i++ {
		p.put(make([]byte, 0, 4096))
	}
	if len(p.free) != 0 || p.freeBytes != 0 {
		t.Fatalf("blocked pool grew: %d buffers, %d bytes", len(p.free), p.freeBytes)
	}
	// take still allocates lazily; release restores normal pooling.
	if b := p.take(); cap(b) != 4096 {
		t.Fatalf("take under block: cap %d", cap(b))
	}
	p.setBlocked(false)
	p.put(make([]byte, 0, 4096))
	if p.freeBytes != 4096 {
		t.Fatalf("release must restore pooling: %d bytes", p.freeBytes)
	}
}

func TestConnectionReleaseIdle(t *testing.T) {
	p := &bufferPool{}
	// Idle connection: buffer returned (and dropped while blocked).
	c := &connection{input: make([]byte, 0, 4096)}
	c.releaseIdle(p)
	if c.input != nil || p.freeBytes != 4096 {
		t.Fatalf("idle connection must release its input buffer: input=%v pooled=%d", c.input != nil, p.freeBytes)
	}
	// Not idle: partial request buffered -> untouched.
	partial := &connection{input: []byte("GET /fe")}
	partial.releaseIdle(p)
	if partial.input == nil {
		t.Fatal("must not drop a partial request")
	}
	// Not idle: response being built.
	out := &connection{output: []byte("HTTP/1.1 200 OK\r\n")}
	out.releaseIdle(p)
	if out.output == nil {
		t.Fatal("must not drop an in-flight response")
	}
	// Not idle: pending writer send.
	pend := &connection{pending: true, input: make([]byte, 0, 4096)}
	pend.releaseIdle(p)
	if pend.input == nil {
		t.Fatal("must not drop a connection with a pending send")
	}
}
