package main

import (
	"bytes"
	"errors"
	"net"
	"syscall"
	"testing"
	"time"

	"github.com/godzie44/go-uring/uring"
)

// Exercise real timeout and accept SQEs, including pressure with no idle client.
func TestAcceptResourceRecovery(t *testing.T) {
	for _, pressure := range []syscall.Errno{syscall.EMFILE, syscall.ENFILE} {
		t.Run(pressure.Error(), func(t *testing.T) {
			listener, err := net.Listen("tcp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			raw, err := listener.(*net.TCPListener).SyscallConn()
			if err != nil {
				t.Fatal(err)
			}
			ring, err := uring.New(8)
			if err != nil {
				t.Fatal(err)
			}
			defer ring.Close()
			s := server{ring: ring, clients: make(map[int]*connection)}
			if err := raw.Control(func(fd uintptr) { s.accept = uring.Accept(fd, syscall.SOCK_CLOEXEC) }); err != nil {
				t.Fatal(err)
			}
			start := time.Now()
			if err := s.complete(0, -int32(pressure)); err != nil {
				t.Fatalf("resource exhaustion must not be fatal: %v", err)
			}
			// Other work can complete while accept is paused.
			if err := s.queue(uring.Nop(), 42); err != nil {
				t.Fatal(err)
			}
			event, err := ring.SubmitAndWaitCQEvents(1)
			if err != nil {
				t.Fatal(err)
			}
			if event.UserData != 42 || event.Res != 0 {
				t.Fatalf("event loop stalled during backoff: %+v", event)
			}
			ring.SeenCQE(event)
			event, err = ring.SubmitAndWaitCQEvents(1)
			if err != nil {
				t.Fatal(err)
			}
			if event.UserData != acceptRetryTag || event.Res != -int32(syscall.ETIME) {
				t.Fatalf("missing accept retry: %+v", event)
			}
			if time.Since(start) < 5*time.Millisecond {
				t.Fatal("accept retried without backoff")
			}
			ring.SeenCQE(event)
			if err := s.complete(acceptRetryTag, -int32(syscall.ETIME)); err != nil {
				t.Fatal(err)
			}
			client, err := net.DialTimeout("tcp4", listener.Addr().String(), time.Second)
			if err != nil {
				t.Fatal(err)
			}
			defer client.Close()
			event, err = ring.SubmitAndWaitCQEvents(1)
			if err != nil {
				t.Fatal(err)
			}
			if event.UserData != 0 || event.Res < 0 {
				t.Fatalf("accept did not recover: %+v", event)
			}
			syscall.Close(int(event.Res))
			ring.SeenCQE(event)
			if err := s.complete(0, -int32(syscall.EBADF)); !errors.Is(err, syscall.EBADF) {
				t.Fatalf("unexpected fatal error swallowed: %v", err)
			}
		})
	}
}

func TestPressureRetiresOnlyIdle(t *testing.T) {
	ring, err := uring.New(16)
	if err != nil {
		t.Fatal(err)
	}
	s := server{ring: ring, clients: make(map[int]*connection)}
	defer ring.Close()
	states := []*connection{
		{in: make([]byte, 16), sent: 1}, // Idle keep-alive.
		{in: make([]byte, 16)},          // First request not read yet.
		{in: make([]byte, 16), sent: 1, used: 1},
		{sent: 1, sending: true},
		{sent: 1, out: []byte("staged response")},
	}
	idleFD := -1
	for i, c := range states {
		pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
		if err != nil {
			t.Fatal(err)
		}
		defer syscall.Close(pair[0])
		defer syscall.Close(pair[1])
		s.clients[pair[0]] = c
		if i == 0 {
			idleFD = pair[0]
			if err := s.receive(idleFD, c); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := s.complete(0, -int32(syscall.EMFILE)); err != nil {
		t.Fatal(err)
	}
	for i, c := range states {
		if c.closing != (i == 0) {
			t.Fatalf("wrong retirement decision for state %d: %+v", i, c)
		}
	}
	if _, err := syscall.GetsockoptInt(idleFD, syscall.SOL_SOCKET, syscall.SO_TYPE); err != nil {
		t.Fatalf("descriptor closed before receive completion: %v", err)
	}
	event, err := ring.SubmitAndWaitCQEvents(1)
	if err != nil {
		t.Fatal(err)
	}
	if event.UserData != uint64(idleFD)+1 || event.Res != 0 {
		t.Fatalf("expected retired receive completion: %+v", event)
	}
	ring.SeenCQE(event)
	if err := s.complete(uint64(idleFD)+1, 0); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.clients[idleFD]; ok {
		t.Fatal("retired connection was re-armed")
	}
	if _, err := syscall.GetsockoptInt(idleFD, syscall.SOL_SOCKET, syscall.SO_TYPE); !errors.Is(err, syscall.EBADF) {
		t.Fatalf("retired descriptor still open: %v", err)
	}
}

// A receive can finish before shutdown while its CQE is still in the ring.
// Answer that request before closing, rather than losing its response.
func TestRetiredReadRace(t *testing.T) {
	pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(pair[0])
	defer syscall.Close(pair[1])
	ring, err := uring.New(8)
	if err != nil {
		t.Fatal(err)
	}
	defer ring.Close()
	c := &connection{in: []byte("GET /missing HTTP/1.1\r\nHost: test\r\n\r\n"), closing: true}
	s := server{ring: ring, app: newApp(nil), clients: map[int]*connection{pair[0]: c}}
	if err := s.complete(uint64(pair[0])+1, int32(len(c.in))); err != nil {
		t.Fatal(err)
	}
	if len(s.ready) != 1 || !bytes.HasPrefix(c.out, []byte("HTTP/1.1 404 ")) {
		t.Fatalf("received request lost during retirement: %q", c.out)
	}
	if err := s.send(pair[0], c); err != nil {
		t.Fatal(err)
	}
	event, err := ring.SubmitAndWaitCQEvents(1)
	if err != nil {
		t.Fatal(err)
	}
	if event.UserData != uint64(pair[0])+1 || event.Res != int32(len(c.out)) {
		t.Fatalf("retired response not sent: %+v", event)
	}
	ring.SeenCQE(event)
	if err := s.complete(uint64(pair[0])+1, event.Res); err != nil {
		t.Fatal(err)
	}
	if len(s.clients) != 0 {
		t.Fatal("connection not closed after final response")
	}
}
