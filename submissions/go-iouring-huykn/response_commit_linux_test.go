package main

import (
	"bytes"
	"testing"
)

func TestMalformedPipelineCommit(t *testing.T) {
	for _, committed := range []bool{true, false} {
		s := testServer(1)
		c := &s.rings[0].conns[0]
		s.serve(0, 0, c, []byte("GET /missing HTTP/1.1\r\n\r\ninvalid\r\n\r\n"))
		s.finishTouched(s.touched[0], committed)
		out := s.buf(c.wire)
		if committed {
			if !bytes.HasPrefix(out, []byte("HTTP/1.1 404 ")) || !bytes.Contains(out, []byte("HTTP/1.1 400 ")) || bytes.Count(out, []byte("HTTP/1.1 ")) != 2 {
				t.Fatalf("pipeline responses lost or reordered: %s", out)
			}
		} else if !bytes.HasPrefix(out, []byte("HTTP/1.1 500 ")) || bytes.Count(out, []byte("HTTP/1.1 ")) != 1 {
			t.Fatalf("rolled-back response escaped: %s", out)
		}
		if c.flags&fCloseAfter == 0 {
			t.Fatal("malformed pipeline kept the connection open")
		}
	}
}
