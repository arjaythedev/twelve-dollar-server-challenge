package main

import "testing"

func TestParseRequest(t *testing.T) {
	two := "POST /posts?x=1 HTTP/1.1\r\nHost: a\r\nauthorization:  Bearer t \r\nContent-Length: 4\r\n\r\nbodyGET /feed HTTP/1.1\r\nConnection: close\r\n\r\n"
	r, n := parseRequest([]byte(two))
	if n != len(two)-len("GET /feed HTTP/1.1\r\nConnection: close\r\n\r\n") || string(r.method) != "POST" ||
		string(r.path) != "/posts" || string(r.auth) != "Bearer t" || string(r.body) != "body" || r.close {
		t.Fatalf("first request: n=%d %+v", n, r)
	}
	r, m := parseRequest([]byte(two[n:]))
	if m != len(two)-n || string(r.path) != "/feed" || !r.close {
		t.Fatalf("pipelined request: n=%d %+v", m, r)
	}
	if _, n := parseRequest([]byte(two[:n-2])); n != 0 { // body not complete yet
		t.Fatal("partial body should wait")
	}
	if _, n := parseRequest([]byte("GET /feed HTTP/1.1\r\nHost")); n != 0 {
		t.Fatal("partial headers should wait")
	}
	if _, n := parseRequest([]byte("garbage\r\n\r\n")); n >= 0 {
		t.Fatal("malformed should fail")
	}
	if _, n := parseRequest([]byte("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")); n >= 0 {
		t.Fatal("chunked should fail")
	}
	if r, _ := parseRequest([]byte("GET / HTTP/1.0\r\n\r\n")); !r.close {
		t.Fatal("HTTP/1.0 closes by default")
	}
}
