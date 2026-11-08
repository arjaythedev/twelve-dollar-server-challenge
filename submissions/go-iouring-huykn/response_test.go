package main

import (
	"bytes"
	"testing"
)

func TestFragmentedAndPipelinedRequests(t *testing.T) {
	first := []byte("POST /posts HTTP/1.1\r\nAuthorization: Bearer abc\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}")
	var req request
	for size := 0; size < len(first); size++ {
		if _, state := parse(first[:size], &req); state != parsedPartial {
			t.Fatalf("fragment %d: state %d", size, state)
		}
	}
	all := append(append([]byte(nil), first...), []byte("GET /feed HTTP/1.1\r\n\r\n")...)
	n, state := parse(all, &req)
	if state != parsedDone || n != len(first) || string(req.body) != `{"body":"x"}` {
		t.Fatalf("first request: n=%d state=%d body=%s", n, state, req.body)
	}
	if _, state := parse(all[n:], &req); state != parsedDone || string(req.path) != "/feed" {
		t.Fatalf("second request: state=%d path=%s", state, req.path)
	}
}

func BenchmarkPostBytes(b *testing.B) {
	a := newApp(nil)
	body, created, author := []byte("synthetic benchmark post"), []byte("2025-12-31T23:59:19.409Z"), []byte("author")
	buf := make([]byte, 0, 8192)
	buf = a.appendPost(buf, 1, body, created, author, 0)
	if allocs := testing.AllocsPerRun(100, func() {
		buf = a.appendPost(buf[:0], 1, body, created, author, 10)
	}); allocs != 0 {
		b.Fatalf("post bytes allocate: %g", allocs)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		buf = a.appendPost(buf[:0], 1, body, created, author, int64(i))
	}
}

func TestRouteValidationCompatibility(t *testing.T) {
	a := newApp(nil)
	for _, tc := range []struct{ method, path, status string }{
		{"GET", "/posts/", "404"},
		{"GET", "/posts/abc", "400"},
		{"GET", "/posts/abc/extra", "404"},
		{"POST", "/posts/abc/like", "401"},
		{"POST", "/posts//like", "404"},
		{"POST", "/posts/a/b/like", "404"},
	} {
		r := request{method: []byte(tc.method), path: []byte(tc.path)}
		response := a.handle(&r, nil)
		if !bytes.HasPrefix(response, []byte("HTTP/1.1 "+tc.status+" ")) {
			t.Fatalf("%s %s: %s", tc.method, tc.path, response)
		}
	}
}
