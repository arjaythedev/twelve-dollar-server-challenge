//go:build linux

package main

import (
	"bytes"
	"golang.org/x/sys/unix"
	"testing"
	"time"
)

func pair(t *testing.T) (int, int) {
	t.Helper()
	p, e := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM|unix.SOCK_NONBLOCK, 0)
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { unix.Close(p[0]); unix.Close(p[1]) })
	return p[0], p[1]
}
func TestConnectionSplitsPipelineReleaseAndClose(t *testing.T) {
	raw := []byte("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n")
	for split := 0; split <= len(raw); split++ {
		fd, peer := pair(t)
		c := connection{fd: int32(fd)}
		p := bufferPool{}
		calls := 0
		handle := func(j job) reply { calls++; return reply{200, []byte(`{"posts":[]}`)} }
		c.receive(raw[:split], &p)
		closed, e := c.drive(&p, handle, time.Now())
		if e != nil || closed {
			t.Fatal(closed, e)
		}
		if split < len(raw) && calls != 0 {
			t.Fatal("premature response")
		}
		c.receive(raw[split:], &p)
		closed, e = c.drive(&p, handle, time.Now())
		if e != nil || closed || calls != 1 {
			t.Fatalf("split %d calls %d closed %v error %v", split, calls, closed, e)
		}
		out := make([]byte, 4096)
		n, e := unix.Read(peer, out)
		if e != nil || !bytes.HasSuffix(out[:n], []byte(`{"posts":[]}`)) {
			t.Fatal(n, e, string(out[:max(0, n)]))
		}
		if c.input != nil || c.output != nil {
			t.Fatal("idle connection retained buffers")
		}
	}
	fd, peer := pair(t)
	c := connection{fd: int32(fd)}
	p := bufferPool{}
	calls := 0
	c.receive(append(raw, []byte("GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")...), &p)
	closed, e := c.drive(&p, func(j job) reply { calls++; return reply{200, []byte(`{}`)} }, time.Now())
	if e != nil || !closed || calls != 2 {
		t.Fatal(calls, closed, e)
	}
	out := make([]byte, 4096)
	n, _ := unix.Read(peer, out)
	if bytes.Count(out[:n], []byte("HTTP/1.1 200 OK")) != 2 || !bytes.Contains(out[:n], []byte("Connection: close")) {
		t.Fatal(string(out[:n]))
	}
	c.release(&p)
	if c.input != nil || c.output != nil {
		t.Fatal("close retained buffer")
	}
}
func TestPendingWriteEAGAINPreservesExactBytes(t *testing.T) {
	fd, peer := pair(t)
	unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_SNDBUF, 1024)
	p := bufferPool{}
	c := connection{fd: int32(fd)}
	want := bytes.Repeat([]byte("0123456789"), 20000)
	c.output = append(p.take(), want...)
	complete, e := c.flush(&p)
	if e != nil || complete || c.offset <= 0 {
		t.Fatal("did not encounter partial write/EAGAIN", complete, c.offset, e)
	}
	var got []byte
	scratch := make([]byte, 8192)
	for i := 0; i < 1000 && c.output != nil; i++ {
		for {
			n, e := unix.Read(peer, scratch)
			if n > 0 {
				got = append(got, scratch[:n]...)
			}
			if e == unix.EAGAIN {
				break
			}
			if e != nil {
				t.Fatal(e)
			}
		}
		_, e = c.flush(&p)
		if e != nil {
			t.Fatal(e)
		}
	}
	for {
		n, e := unix.Read(peer, scratch)
		if n > 0 {
			got = append(got, scratch[:n]...)
		}
		if e == unix.EAGAIN {
			break
		}
		if e != nil {
			t.Fatal(e)
		}
	}
	if !bytes.Equal(got, want) || c.output != nil || c.offset != 0 {
		t.Fatal("partial write lost/repeated bytes", len(got), len(want))
	}
}
func TestDisconnectMidRequestReleasesWithoutDispatch(t *testing.T) {
	fd, peer := pair(t)
	p := bufferPool{}
	c := connection{fd: int32(fd)}
	unix.Write(peer, []byte("POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\n{"))
	unix.Close(peer)
	calls := 0
	closed, e := c.readReady(&p, func(j job) reply { calls++; return reply{200, nil} }, time.Now())
	if e != nil || !closed || calls != 0 {
		t.Fatal(closed, e, calls)
	}
	c.release(&p)
	if c.input != nil || c.output != nil {
		t.Fatal("disconnect leaked buffer")
	}
}

func TestExpectContinueBeforeFinalResponse(t *testing.T) {
	for _, split := range []bool{false, true} {
		fd, peer := pair(t)
		c := connection{fd: int32(fd)}
		p := bufferPool{}
		calls := 0
		header := []byte("POST /posts HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 12\r\n\r\n")
		body := []byte(`{"body":"x"}`)
		if split {
			c.receive(header, &p)
			closed, e := c.drive(&p, func(j job) reply { calls++; return reply{401, []byte(`{"error":"missing bearer token"}`)} }, time.Now())
			if closed || e != nil || calls != 0 {
				t.Fatal(closed, e, calls)
			}
			c.receive(body, &p)
		} else {
			c.receive(append(header, body...), &p)
		}
		closed, e := c.drive(&p, func(j job) reply { calls++; return reply{401, []byte(`{"error":"missing bearer token"}`)} }, time.Now())
		if closed || e != nil || calls != 1 {
			t.Fatal(closed, e, calls)
		}
		b := make([]byte, 4096)
		n, e := unix.Read(peer, b)
		if e != nil || !bytes.HasPrefix(b[:n], []byte("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 401 Unauthorized\r\n")) {
			t.Fatal(string(b[:max(0, n)]), e)
		}
	}
}
