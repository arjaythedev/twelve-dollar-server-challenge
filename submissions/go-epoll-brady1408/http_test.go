package main

import (
	"strings"
	"testing"
)

func TestParseGetKeepAlive(t *testing.T) {
	raw := "GET /feed?x=1 HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer abc\r\n\r\n"
	req, n, st := parseRequest([]byte(raw))
	if st != parseComplete || n != len(raw) {
		t.Fatalf("st=%v n=%d", st, n)
	}
	if req.method != methodGET || string(req.path) != "/feed" || string(req.auth) != "Bearer abc" || req.connClose {
		t.Fatalf("%+v", req)
	}
}

func TestParsePostWithBody(t *testing.T) {
	raw := "POST /posts HTTP/1.1\r\ncontent-length: 12\r\nContent-Type: application/json\r\n\r\n{\"body\":\"x\"}"
	req, n, st := parseRequest([]byte(raw))
	if st != parseComplete || n != len(raw) || string(req.body) != `{"body":"x"}` || req.method != methodPOST {
		t.Fatalf("st=%v n=%d body=%q", st, n, req.body)
	}
}

func TestParseSplitAcrossReads(t *testing.T) {
	raw := "POST /posts HTTP/1.1\r\nContent-Length: 12\r\n\r\n{\"body\":"
	if _, _, st := parseRequest([]byte(raw)); st != parseNeedMore {
		t.Fatalf("partial body: st=%v", st)
	}
	if _, _, st := parseRequest([]byte("GET /feed HTTP/1.1\r\nHost")); st != parseNeedMore {
		t.Fatalf("partial headers: st=%v", st)
	}
}

func TestParsePipelined(t *testing.T) {
	a := "GET /feed HTTP/1.1\r\n\r\n"
	b := "GET /posts/1 HTTP/1.1\r\nConnection: close\r\n\r\n"
	buf := []byte(a + b)
	req, n, st := parseRequest(buf)
	if st != parseComplete || n != len(a) || string(req.path) != "/feed" {
		t.Fatalf("first: st=%v n=%d path=%s", st, n, req.path)
	}
	req, n, st = parseRequest(buf[n:])
	if st != parseComplete || n != len(b) || string(req.path) != "/posts/1" || !req.connClose {
		t.Fatalf("second: st=%v n=%d %+v", st, n, req)
	}
}

func TestParseFlags(t *testing.T) {
	req, _, st := parseRequest([]byte("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n"))
	if st != parseComplete || !req.chunked || !req.expectContinue {
		t.Fatalf("st=%v %+v", st, req)
	}
	if req, _, st := parseRequest([]byte("GET / HTTP/1.0\r\n\r\n")); st != parseComplete || !req.connClose {
		t.Fatalf("http/1.0 should close: st=%v %+v", st, req)
	}
}

func TestParseLimitsAndGarbage(t *testing.T) {
	if _, _, st := parseRequest([]byte("GET /feed HTTP/1.1\r\nX: " + strings.Repeat("a", maxHeaderBytes) + "\r\n")); st != parseHeadersTooLarge {
		t.Fatalf("st=%v", st)
	}
	if _, _, st := parseRequest([]byte("POST /posts HTTP/1.1\r\nContent-Length: 99999\r\n\r\n")); st != parseBodyTooLarge {
		t.Fatalf("st=%v", st)
	}
	for _, bad := range []string{"GET\r\n\r\n", "GET /feed\r\n\r\n", "GET /feed FTP/9\r\n\r\n", "POST /posts HTTP/1.1\r\nContent-Length: abc\r\n\r\n", "GET /feed HTTP/1.1\r\nNoColon\r\n\r\n"} {
		if _, _, st := parseRequest([]byte(bad)); st != parseBad {
			t.Errorf("%q: st=%v, want parseBad", bad, st)
		}
	}
}

func TestAppendResponse(t *testing.T) {
	got := string(appendResponse(nil, 201, []byte(`{"a":1}`), false))
	want := "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 7\r\nConnection: keep-alive\r\n\r\n{\"a\":1}"
	if got != want {
		t.Fatalf("got %q", got)
	}
	if got := string(appendResponse(nil, 404, []byte(`{}`), true)); !strings.Contains(got, "HTTP/1.1 404 Not Found\r\n") || !strings.Contains(got, "Connection: close\r\n") {
		t.Fatalf("got %q", got)
	}
}
