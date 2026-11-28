package main

import (
	"bytes"
	"strconv"
)

const (
	maxHeader = 16 << 10
	maxBody   = 16 << 10
)

var (
	crlf              = []byte("\r\n")
	headerEnd         = []byte("\r\n\r\n")
	hContentLength    = []byte("Content-Length")
	hAuthorization    = []byte("Authorization")
	hConnection       = []byte("Connection")
	hTransferEncoding = []byte("Transfer-Encoding")
)

// request fields point into the read buffer and are only valid while it is being handled.
type request struct {
	method, path, auth, body []byte
	close                    bool
}

// parseRequest reads one HTTP/1.x request from the front of b. It returns the bytes it used,
// 0 when the request is still incomplete, or -1 when it is malformed or too large.
func parseRequest(b []byte) (r request, n int) {
	end := bytes.Index(b, headerEnd)
	if end < 0 {
		if len(b) > maxHeader {
			return r, -1
		}
		return r, 0
	}
	if end > maxHeader {
		return r, -1
	}
	line, headers, _ := bytes.Cut(b[:end], crlf)
	method, rest, ok1 := bytes.Cut(line, []byte(" "))
	target, proto, ok2 := bytes.Cut(rest, []byte(" "))
	if !ok1 || !ok2 || len(method) == 0 || len(target) == 0 || target[0] != '/' || !bytes.HasPrefix(proto, []byte("HTTP/1.")) {
		return r, -1
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.method, r.path = method, target
	r.close = string(proto) == "HTTP/1.0"
	length := 0
	for len(headers) > 0 {
		var h []byte
		h, headers, _ = bytes.Cut(headers, crlf)
		name, value, ok := bytes.Cut(h, []byte(":"))
		if !ok {
			return r, -1
		}
		value = bytes.TrimSpace(value)
		switch {
		case bytes.EqualFold(name, hContentLength):
			v, err := strconv.Atoi(string(value))
			if err != nil || v < 0 || v > maxBody {
				return r, -1
			}
			length = v
		case bytes.EqualFold(name, hAuthorization):
			r.auth = value
		case bytes.EqualFold(name, hConnection):
			if bytes.EqualFold(value, []byte("close")) {
				r.close = true
			} else if bytes.EqualFold(value, []byte("keep-alive")) {
				r.close = false
			}
		case bytes.EqualFold(name, hTransferEncoding):
			return r, -1 // chunked request bodies are not supported
		}
	}
	n = end + len(headerEnd) + length
	if len(b) < n {
		return r, 0
	}
	r.body = b[end+len(headerEnd) : n]
	return r, n
}

func appendResponse(out []byte, status int, body []byte, close bool) []byte {
	out = append(out, "HTTP/1.1 "...)
	out = append(out, statusLine(status)...)
	out = append(out, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	out = strconv.AppendInt(out, int64(len(body)), 10)
	if close {
		out = append(out, "\r\nConnection: close"...)
	}
	out = append(out, "\r\n\r\n"...)
	return append(out, body...)
}

func statusLine(status int) string {
	switch status {
	case 200:
		return "200 OK"
	case 201:
		return "201 Created"
	case 400:
		return "400 Bad Request"
	case 401:
		return "401 Unauthorized"
	case 404:
		return "404 Not Found"
	case 503:
		return "503 Service Unavailable"
	}
	return "500 Internal Server Error"
}
