//go:build linux && cgo_sqlite

package main

import (
	"bytes"
	"golang.org/x/sys/unix"
	"strings"
	"testing"
	"time"
)

func TestPipelinedCReadsOnOneConnection(t *testing.T) {
	a, _ := fixture(t, testSchema)
	fd, peer := pair(t)
	p := bufferPool{}
	c := connection{fd: int32(fd)}
	c.receive([]byte("GET /feed HTTP/1.1\r\nHost: x\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n"), &p)
	closed, err := c.drive(&p, a.handle, time.Now())
	if closed || err != nil {
		t.Fatal(closed, err)
	}
	out := make([]byte, 8192)
	n, err := unix.Read(peer, out)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Count(out[:n], []byte("HTTP/1.1 200 OK")) != 2 || !bytes.Contains(out[:n], []byte(`{"posts":[{"id":1`)) || !bytes.Contains(out[:n], []byte(`{"post":{"id":1`)) {
		t.Fatalf("pipeline: %q", out[:n])
	}
}

func TestSlowReaderOutputSurvivesAnotherCRead(t *testing.T) {
	a, _ := fixture(t, testSchema)
	for id := 2; id <= 21; id++ {
		if _, err := a.conn.ExecContext(ctx, "INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,'2025-01-01T00:00:00.000Z')", id, strings.Repeat("<", 500)); err != nil {
			t.Fatal(err)
		}
	}
	fd, peer := pair(t)
	if err := unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_SNDBUF, 1024); err != nil {
		t.Fatal(err)
	}
	p := bufferPool{}
	slow := connection{fd: int32(fd)}
	slow.receive([]byte("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n"), &p)
	closed, err := slow.drive(&p, a.handle, time.Now())
	if closed || err != nil || slow.output == nil || slow.offset == 0 {
		t.Fatalf("no partial response: closed=%v err=%v offset=%d", closed, err, slow.offset)
	}
	want := bytes.Clone(slow.output)
	fastFD, fastPeer := pair(t)
	fast := connection{fd: int32(fastFD)}
	fast.receive([]byte("GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n"), &p)
	closed, err = fast.drive(&p, a.handle, time.Now())
	if closed || err != nil {
		t.Fatal(closed, err)
	}
	other := make([]byte, 4096)
	n, err := unix.Read(fastPeer, other)
	if err != nil || !bytes.Contains(other[:n], []byte(`{"post":{"id":1`)) {
		t.Fatal(n, err)
	}
	if !bytes.Equal(slow.output, want) {
		t.Fatal("slow output changed after another C read")
	}
	got := make([]byte, 0, len(want))
	buf := make([]byte, 8192)
	for i := 0; i < 1000 && slow.output != nil; i++ {
		for {
			n, e := unix.Read(peer, buf)
			if n > 0 {
				got = append(got, buf[:n]...)
			}
			if e == unix.EAGAIN {
				break
			}
			if e != nil {
				t.Fatal(e)
			}
		}
		_, e := slow.flush(&p)
		if e != nil {
			t.Fatal(e)
		}
	}
	for {
		n, e := unix.Read(peer, buf)
		if n > 0 {
			got = append(got, buf[:n]...)
		}
		if e == unix.EAGAIN {
			break
		}
		if e != nil {
			t.Fatal(e)
		}
	}
	if slow.output != nil || !bytes.Equal(got, want) {
		t.Fatalf("partial output corrupted: got %d want %d", len(got), len(want))
	}
}
