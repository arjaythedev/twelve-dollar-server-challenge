package main

import (
	"testing"
)

func TestResponseBufferClasses(t *testing.T) {
	s := testServer(1)
	s.req.path = []byte("/feed")
	feed := s.takeResponse()
	if cap(s.buf(feed)) != outCap {
		t.Fatal("feed lost its existing reservation")
	}
	s.give(feed)
	s.req.path = []byte("/posts/500000")
	small := s.takeResponse()
	if small == feed || cap(s.buf(small)) != partCap {
		t.Fatal("small response borrowed a feed buffer")
	}
	s.pool[small-1] = append(s.buf(small), make([]byte, 2048)...)
	s.give(small)
	if reused := s.takeResponse(); reused != small || cap(s.buf(reused)) < 2048 {
		t.Fatal("grown response buffer was not reusable")
	}
}

// Withhold send completions: each connection must retain its own response.
func TestSmallResponseBacklog(t *testing.T) {
	s := testServer(4096)
	for i := range s.rings[0].conns {
		s.answers = answersPerPass
		c := &s.rings[0].conns[i]
		s.serve(0, uint32(i), c, []byte("GET /missing HTTP/1.1\r\n\r\n"))
		if cap(s.buf(c.out)) > partCap {
			t.Fatalf("small response retains %d bytes, want at most %d", cap(s.buf(c.out)), partCap)
		}
	}
}

func BenchmarkSmallResponseBacklog(b *testing.B) {
	const connections = 4096
	s := testServer(connections)
	wire := []byte("GET /missing HTTP/1.1\r\n\r\n")
	for i := range s.rings[0].conns {
		s.answers = answersPerPass
		s.serve(0, uint32(i), &s.rings[0].conns[i], wire)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c := &s.rings[0].conns[i%connections]
		s.pool[c.out-1] = s.buf(c.out)[:0]
		s.answers = answersPerPass
		s.serve(0, uint32(i%connections), c, wire)
	}
	b.StopTimer()
	retained := 0
	for _, c := range s.rings[0].conns {
		retained += cap(s.buf(c.out))
	}
	b.ReportMetric(float64(retained)/connections, "retained-B/conn")
}
