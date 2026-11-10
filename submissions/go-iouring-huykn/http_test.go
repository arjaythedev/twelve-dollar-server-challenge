package main

import (
	"bytes"
	"testing"
)

func TestHTTPFraming(t *testing.T) {
	for _, wire := range []string{
		"GET /feed HTTP/1.1junk\r\n\r\n",
		"POST /posts HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}",
		"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
		"GET /feed HTTP/1.1\r\n" + string(bytes.Repeat([]byte{'x'}, maxHead)) + "\r\n\r\n",
	} {
		var req request
		if _, state := parse([]byte(wire), &req); state != parsedBad {
			t.Errorf("accepted malformed framing (%d bytes): state %d", len(wire), state)
		}
	}
	// Header limits apply to the header, independently of body and pipeline size.
	body := bytes.Repeat([]byte{'x'}, maxHead+1)
	wire := append([]byte("POST /posts HTTP/1.1\r\nContent-Length: 16385\r\n\r\n"), body...)
	wire = append(wire, "GET /feed HTTP/1.1\r\n\r\n"...)
	var req request
	n, state := parse(wire, &req)
	if state != parsedDone || !bytes.Equal(req.body, body) || n != len(wire)-len("GET /feed HTTP/1.1\r\n\r\n") {
		t.Fatalf("large body/pipeline: n=%d state=%d body=%d", n, state, len(req.body))
	}
}

func TestV2FramingCompatibility(t *testing.T) {
	for _, tc := range []struct {
		name, wire, method, path, auth, body string
	}{
		{"LF and query", "GET /feed?limit=20 HTTP/1.1\nIgnored line\n\n", "GET", "/feed", "", ""},
		{"HTTP 1.0", "GET /feed HTTP/1.0\r\n\r\n", "GET", "/feed", "", ""},
		{"header case and whitespace", "POST /posts HTTP/1.1\r\naUtHoRiZaTiOn: \tBearer test\t \r\nCoNtEnT-LeNgTh: \t12 \r\n\r\n{\"body\":\"x\"}", "POST", "/posts", "Bearer test", `{"body":"x"}`},
		{"last authorization", "GET /feed HTTP/1.1\r\nAuthorization: first\r\nAuthorization: second\r\nContent-Length: 000000000\r\n\r\n", "GET", "/feed", "second", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var req request
			n, state := parse([]byte(tc.wire), &req)
			if state != parsedDone || n != len(tc.wire) || string(req.method) != tc.method || string(req.path) != tc.path || string(req.auth) != tc.auth || string(req.body) != tc.body || req.hasAuth != (tc.auth != "") {
				t.Fatalf("state=%d used=%d request=%+v", state, n, req)
			}
		})
	}
}

func BenchmarkHTTPFraming(b *testing.B) {
	for _, wire := range []string{
		"GET /feed HTTP/1.1\r\nHost: test\r\n\r\n",
		"POST /posts HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer test\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}",
	} {
		b.Run(wire[:3], func(b *testing.B) {
			buf := []byte(wire)
			var req request
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if n, state := parse(buf, &req); state != parsedDone || n != len(buf) {
					b.Fatal("framing failed")
				}
			}
		})
	}
}
