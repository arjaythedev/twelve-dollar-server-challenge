package main

import (
	"bytes"
	"strconv"
)

// A minimal HTTP/1.1 request parser: just enough for this API (Content-Length bodies,
// keep-alive, Expect: 100-continue). All slices point into the connection's input.

const (
	maxHeaderBytes = 16 << 10
	maxBodyBytes   = 1 << 20
)

type request struct {
	method, path, auth, body []byte
	hasAuth                  bool
	keepAlive                bool
	http10                   bool
}

// Parse results besides "complete".
const (
	parseOK = iota
	parseIncomplete
	parseNeedContinue // headers done, waiting on a body the client wants a 100 Continue for
	parseBad
	parseTooLarge
	parseUnsupported
)

var (
	crlf     = []byte("\r\n")
	crlfcrlf = []byte("\r\n\r\n")
)

// parseRequest returns the result and, for parseOK, how many bytes the request used.
func parseRequest(b []byte, r *request) (int, int) {
	he := bytes.Index(b, crlfcrlf)
	if he < 0 {
		if len(b) > maxHeaderBytes {
			return 0, parseTooLarge
		}
		return 0, parseIncomplete
	}
	head := b[:he+2]
	le := bytes.Index(head, crlf)
	line := head[:le]
	sp1 := bytes.IndexByte(line, ' ')
	sp2 := bytes.LastIndexByte(line, ' ')
	if sp1 <= 0 || sp2 <= sp1+1 {
		return 0, parseBad
	}
	*r = request{method: line[:sp1], path: line[sp1+1 : sp2]}
	switch string(line[sp2+1:]) {
	case "HTTP/1.1":
		r.keepAlive = true
	case "HTTP/1.0":
		r.http10 = true
	default:
		return 0, parseBad
	}

	contentLength, expectContinue := 0, false
	for rest := head[le+2:]; len(rest) > 0; {
		e := bytes.Index(rest, crlf)
		h := rest[:e]
		rest = rest[e+2:]
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return 0, parseBad
		}
		name, val := h[:colon], trimOWS(h[colon+1:])
		switch {
		case equalFold(name, "authorization"):
			r.auth, r.hasAuth = val, true
		case equalFold(name, "content-length"):
			n, err := strconv.Atoi(string(val))
			if err != nil || n < 0 {
				return 0, parseBad
			}
			if n > maxBodyBytes {
				return 0, parseTooLarge
			}
			contentLength = n
		case equalFold(name, "connection"):
			if containsFold(val, "close") {
				r.keepAlive = false
			} else if containsFold(val, "keep-alive") {
				r.keepAlive = true
			}
		case equalFold(name, "transfer-encoding"):
			return 0, parseUnsupported
		case equalFold(name, "expect"):
			expectContinue = equalFold(val, "100-continue")
		}
	}

	total := he + 4 + contentLength
	if len(b) < total {
		if expectContinue {
			return 0, parseNeedContinue
		}
		return 0, parseIncomplete
	}
	r.body = b[he+4 : total]
	if q := bytes.IndexByte(r.path, '?'); q >= 0 {
		r.path = r.path[:q]
	}
	return total, parseOK
}

func trimOWS(b []byte) []byte {
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t') {
		b = b[1:]
	}
	for len(b) > 0 && (b[len(b)-1] == ' ' || b[len(b)-1] == '\t') {
		b = b[:len(b)-1]
	}
	return b
}

// equalFold compares ASCII case-insensitively against a lowercase string.
func equalFold(b []byte, lower string) bool {
	if len(b) != len(lower) {
		return false
	}
	for i := range b {
		c := b[i]
		if 'A' <= c && c <= 'Z' {
			c += 'a' - 'A'
		}
		if c != lower[i] {
			return false
		}
	}
	return true
}

func containsFold(b []byte, lower string) bool {
	for i := 0; i+len(lower) <= len(b); i++ {
		if equalFold(b[i:i+len(lower)], lower) {
			return true
		}
	}
	return false
}

// --- responses ------------------------------------------------------------------------------

func statusLine(status int) string {
	switch status {
	case 200:
		return "HTTP/1.1 200 OK\r\n"
	case 201:
		return "HTTP/1.1 201 Created\r\n"
	case 400:
		return "HTTP/1.1 400 Bad Request\r\n"
	case 401:
		return "HTTP/1.1 401 Unauthorized\r\n"
	case 404:
		return "HTTP/1.1 404 Not Found\r\n"
	case 413:
		return "HTTP/1.1 413 Content Too Large\r\n"
	case 501:
		return "HTTP/1.1 501 Not Implemented\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	}
	return "HTTP/1.1 500 Internal Server Error\r\n"
}

const (
	connKeep  = 0
	connClose = 1
	conn10    = 2 // HTTP/1.0 keep-alive must be confirmed explicitly
)

var continueResponse = []byte("HTTP/1.1 100 Continue\r\n\r\n")

func appendHeader(out []byte, status, bodyLen, connMode int) []byte {
	out = append(out, statusLine(status)...)
	out = append(out, "Content-Type: application/json\r\nContent-Length: "...)
	out = strconv.AppendInt(out, int64(bodyLen), 10)
	switch connMode {
	case connClose:
		out = append(out, "\r\nConnection: close"...)
	case conn10:
		out = append(out, "\r\nConnection: keep-alive"...)
	}
	return append(out, "\r\n\r\n"...)
}

func appendResponse(out []byte, status int, body []byte, connMode int) []byte {
	return append(appendHeader(out, status, len(body), connMode), body...)
}

// Header room reserved in front of bodies rendered in place by C, then closed up with a copy.
const headerRoom = 128

var (
	errNotFound       = []byte(`{"error":"not found"}`)
	errInvalidID      = []byte(`{"error":"invalid post id"}`)
	errPostNotFound   = []byte(`{"error":"post not found"}`)
	errMissingToken   = []byte(`{"error":"missing bearer token"}`)
	errInvalidToken   = []byte(`{"error":"invalid or expired token"}`)
	errInvalidPayload = []byte(`{"error":"invalid token payload"}`)
	errMalformedJSON  = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired   = []byte(`{"error":"body is required"}`)
	errBodyTooLong    = []byte(`{"error":"body must be at most 500 characters"}`)
	errInternal       = []byte(`{"error":"internal server error"}`)
	errBadRequest     = []byte(`{"error":"bad request"}`)
	errTooLarge       = []byte(`{"error":"request too large"}`)
	errUnsupported    = []byte(`{"error":"transfer-encoding not supported"}`)
)
