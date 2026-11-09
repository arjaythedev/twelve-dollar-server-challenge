package main

import "testing"

func BenchmarkReceivePath(b *testing.B) {
	s := testServer(1)
	c := &s.rings[0].conns[0]
	wire := []byte("GET /missing HTTP/1.1\r\n\r\n")
	s.received(0, 0, c, wire)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.answers = answersPerPass
		s.pool[c.out-1] = s.buf(c.out)[:0]
		s.received(0, 0, c, wire)
	}
}
