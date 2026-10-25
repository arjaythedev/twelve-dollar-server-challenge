package main

import (
	"strings"
	"testing"
)

func TestHeaderFitsReserve(t *testing.T) {
	// The whole zero-copy send rests on the header fitting in front of the body. The longest
	// header this file can emit is the longest status line plus every optional field.
	longest := len(statusLine(431)) +
		len("Content-Type: application/json\r\nContent-Length: ") + len("8192") +
		len("\r\nConnection: close") + len("\r\n\r\n")
	if longest > hdrReserve {
		t.Fatalf("longest header is %d bytes, hdrReserve is %d", longest, hdrReserve)
	}
}

func TestParseSimpleGet(t *testing.T) {
	raw := "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n"
	var p parsed
	st := parseRequest([]byte(raw), &p)
	if st != parseDone {
		t.Fatalf("state = %v, want parseDone", st)
	}
	if !eq(p.req.method, "GET") || !eq(p.req.path, "/feed") {
		t.Fatalf("method/path = %q %q", p.req.method, p.req.path)
	}
	if !p.keepAlive {
		t.Fatal("HTTP/1.1 should default to keep-alive")
	}
	if p.n != len(raw) {
		t.Fatalf("consumed %d of %d", p.n, len(raw))
	}
	if len(p.req.body) != 0 || p.req.auth != nil {
		t.Fatalf("unexpected body %q or auth %q", p.req.body, p.req.auth)
	}
}

func TestParseHeadersAndBody(t *testing.T) {
	raw := "POST /posts HTTP/1.1\r\n" +
		"AUTHORIZATION:   Bearer abc.def.ghi  \r\n" +
		"Content-Length: 13\r\n" +
		"Connection: close\r\n\r\n" +
		`{"body":"x"}` + "!"
	var p parsed
	st := parseRequest([]byte(raw), &p)
	if st != parseDone {
		t.Fatalf("state = %v", st)
	}
	if !eq(p.req.auth, "Bearer abc.def.ghi") {
		t.Fatalf("auth = %q", p.req.auth)
	}
	if !eq(p.req.body, `{"body":"x"}!`) {
		t.Fatalf("body = %q", p.req.body)
	}
	if p.keepAlive {
		t.Fatal("Connection: close was ignored")
	}
	if p.n != len(raw) {
		t.Fatalf("consumed %d of %d", p.n, len(raw))
	}
}

func TestParseIncomplete(t *testing.T) {
	cases := []string{
		"",
		"GET /feed HT",
		"GET /feed HTTP/1.1\r\nHost: x\r\n",
		"GET /feed HTTP/1.1\r\nHost: x\r\n\r",
		"POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345",
	}
	var p parsed
	for _, raw := range cases {
		if st := parseRequest([]byte(raw), &p); st != parseIncomplete {
			t.Errorf("%q: state = %v, want parseIncomplete", raw, st)
		}
	}
}

func TestParsePipelined(t *testing.T) {
	one := "GET /feed HTTP/1.1\r\n\r\n"
	raw := []byte(one + "GET /health HTTP/1.1\r\n\r\n")
	var p, p2 parsed
	st := parseRequest(raw, &p)
	if st != parseDone || p.n != len(one) {
		t.Fatalf("first: state %v, n %d, want parseDone and %d", st, p.n, len(one))
	}
	st = parseRequest(raw[p.n:], &p2)
	if st != parseDone || !eq(p2.req.path, "/health") {
		t.Fatalf("second: state %v, path %q", st, p2.req.path)
	}
}

func TestParseRejections(t *testing.T) {
	cases := []struct {
		raw  string
		want int
	}{
		{"GET\r\n\r\n", 400},
		{"GET /feed\r\n\r\n", 400},
		{"GET /feed HTTP/2.0\r\n\r\n", 505},
		{"GET /feed HTTP/1.1\r\nbroken\r\n\r\n", 400},
		{"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
		{"POST /posts HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400},
		{"POST /posts HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400},
		{"POST /posts HTTP/1.1\r\nContent-Length: 99999\r\n\r\n", 413},
		{"GET " + strings.Repeat("/x", maxHeadBytes) + " HTTP/1.1\r\n", 431},
	}
	var p parsed
	for _, c := range cases {
		st := parseRequest([]byte(c.raw), &p)
		if st != parseFailed {
			t.Errorf("%.40q: state = %v, want parseFailed", c.raw, st)
			continue
		}
		if p.status != c.want {
			t.Errorf("%.40q: status = %d, want %d", c.raw, p.status, c.want)
		}
		if p.keepAlive {
			t.Errorf("%.40q: a rejected request must not keep the connection", c.raw)
		}
	}
}

func TestParseQueryStringIsNotPartOfThePath(t *testing.T) {
	var p parsed
	st := parseRequest([]byte("GET /posts/7?x=1&y=2 HTTP/1.1\r\n\r\n"), &p)
	if st != parseDone || !eq(p.req.path, "/posts/7") {
		t.Fatalf("state %v, path %q", st, p.req.path)
	}
}

func TestParseHTTP10AndKeepAlive(t *testing.T) {
	var p parsed
	st := parseRequest([]byte("GET /feed HTTP/1.0\r\n\r\n"), &p)
	if st != parseDone || p.keepAlive {
		t.Fatalf("HTTP/1.0 defaults to close: state %v, keepAlive %v", st, p.keepAlive)
	}
	st = parseRequest([]byte("GET /feed HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n"), &p)
	if st != parseDone || !p.keepAlive {
		t.Fatalf("HTTP/1.0 with keep-alive: state %v, keepAlive %v", st, p.keepAlive)
	}
}

func TestParseExpect100(t *testing.T) {
	raw := "POST /posts HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 12\r\n\r\n"
	var p parsed
	st := parseRequest([]byte(raw), &p)
	if st != parseIncomplete {
		t.Fatalf("state = %v, want parseIncomplete", st)
	}
	if !p.expect100 {
		t.Fatal("expect100 not reported, so the client would wait for a 100 that never comes")
	}
}

func TestFrameResponse(t *testing.T) {
	buf := make([]byte, hdrReserve+64)
	body := copy(buf[hdrReserve:], `{"status":"ok"}`)
	start := frameResponse(buf, body, 200, true)
	got := string(buf[start : hdrReserve+body])
	want := "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n" +
		`{"status":"ok"}`
	if got != want {
		t.Fatalf("got %q\nwant %q", got, want)
	}

	start = frameResponse(buf, body, 201, false)
	got = string(buf[start : hdrReserve+body])
	if !strings.Contains(got, "HTTP/1.1 201 Created\r\n") || !strings.Contains(got, "Connection: close\r\n") {
		t.Fatalf("got %q", got)
	}
	if !strings.HasSuffix(got, `{"status":"ok"}`) {
		t.Fatalf("framing moved the body: %q", got)
	}
}

func TestTokenPresent(t *testing.T) {
	cases := []struct {
		value, tok string
		want       bool
	}{
		{"close", "close", true},
		{"keep-alive, Upgrade", "upgrade", true},
		{"Keep-Alive", "keep-alive", true},
		{"closed", "close", false},
		{"", "close", false},
		{"a, b , close", "close", true},
	}
	for _, c := range cases {
		if got := tokenPresent([]byte(c.value), c.tok); got != c.want {
			t.Errorf("tokenPresent(%q, %q) = %v", c.value, c.tok, got)
		}
	}
}

func TestParseNoAllocations(t *testing.T) {
	raw := []byte("GET /posts/12345 HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer a.b.c\r\n" +
		"User-Agent: k6/2.3.0\r\nAccept: */*\r\n\r\n")
	// p lives outside the closure: parseRequest must not allocate, and a parsed declared
	// inside would be a per-run allocation of the test's own making.
	var p parsed
	allocs := testing.AllocsPerRun(200, func() {
		if parseRequest(raw, &p) != parseDone {
			t.Fatal("parse failed")
		}
	})
	if allocs != 0 {
		t.Fatalf("parseRequest allocates %.1f times per call", allocs)
	}
}
