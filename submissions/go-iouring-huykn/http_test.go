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
