package main

import "testing"

func TestParseRequest(t *testing.T) {
	tests := []struct {
		name                   string
		in                     string
		wantN                  int
		method, path, auth, bd string
		close                  bool
	}{
		{name: "get", in: "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n", wantN: 31, method: "GET", path: "/feed"},
		{name: "query stripped", in: "GET /feed?x=1 HTTP/1.1\r\n\r\n", wantN: 26, method: "GET", path: "/feed"},
		{name: "incomplete head", in: "GET /feed HTTP/1.1\r\nHost:", wantN: 0},
		{name: "body", in: "POST /posts HTTP/1.1\r\nContent-Length: 5\r\nauthorization: Bearer t\r\n\r\nhello", wantN: 73, method: "POST", path: "/posts", auth: "Bearer t", bd: "hello"},
		{name: "incomplete body", in: "POST /posts HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel", wantN: 0},
		{name: "pipelined", in: "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n", wantN: 19, method: "GET", path: "/a"},
		{name: "connection close", in: "GET / HTTP/1.1\r\nConnection: close\r\n\r\n", wantN: 37, method: "GET", path: "/", close: true},
		{name: "http/1.0", in: "GET / HTTP/1.0\r\n\r\n", wantN: 18, method: "GET", path: "/", close: true},
		{name: "http/1.0 keep-alive", in: "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", wantN: 42, method: "GET", path: "/"},
		{name: "bad request line", in: "GARBAGE\r\n\r\n", wantN: -1},
		{name: "bad content-length", in: "POST / HTTP/1.1\r\nContent-Length: x\r\n\r\n", wantN: -1},
		{name: "body too large", in: "POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", wantN: -1},
		{name: "chunked", in: "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", wantN: -1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			r, n := parseRequest([]byte(tt.in))
			if n != tt.wantN {
				t.Fatalf("n = %d, want %d", n, tt.wantN)
			}
			if n <= 0 {
				return
			}
			if string(r.method) != tt.method || string(r.path) != tt.path || string(r.auth) != tt.auth || string(r.body) != tt.bd || r.close != tt.close {
				t.Fatalf("got %q %q auth=%q body=%q close=%v", r.method, r.path, r.auth, r.body, r.close)
			}
		})
	}
}

func TestParseRequestHeaderTooLarge(t *testing.T) {
	in := make([]byte, maxHeader+1)
	for i := range in {
		in[i] = 'a'
	}
	if _, n := parseRequest(in); n != -1 {
		t.Fatalf("n = %d, want -1", n)
	}
}

func TestAppendResponse(t *testing.T) {
	got := string(appendResponse(nil, 201, []byte(`{"a":1}`), false))
	want := "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}"
	if got != want {
		t.Fatalf("got %q", got)
	}
	got = string(appendResponse(nil, 400, nil, true))
	want = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
	if got != want {
		t.Fatalf("got %q", got)
	}
}
