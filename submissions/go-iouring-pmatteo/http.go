package main

// HTTP/1.1 request parsing and response framing, by hand.
//
// Scope is the spec and nothing else: a request line, the four headers that change behaviour, and
// a fixed response frame. No chunked bodies, no trailers, no header continuation lines. Every
// field is a view into the caller's buffer, so a parse allocates nothing.
//
// The scans go through the bytes package rather than a byte loop: IndexByte and Index compile to
// vector intrinsics, and the header scan is the one place in a request where the length is not
// bounded by a few bytes.

import "bytes"

const (
	// hdrReserve is kept free at the front of a response buffer. The body is written after it
	// and the header is then written backwards into the gap, so the send needs no copy of the
	// body. 128 is checked against the longest header this file can emit in http_test.go.
	hdrReserve = 128

	maxHeadBytes = 4096 // request line plus headers
	maxBodyBytes = 8192 // Content-Length ceiling; a post is capped at 500 characters
	maxReqBytes  = maxHeadBytes + maxBodyBytes
)

const (
	errHeadTooLarge = `{"error":"request header too large"}`
	errBodyTooLarge = `{"error":"request body too large"}`
	errBadRequest   = `{"error":"malformed request"}`
	errUnsupported  = `{"error":"unsupported transfer encoding"}`
)

const continue100 = "HTTP/1.1 100 Continue\r\n\r\n"

type parseState uint8

const (
	parseIncomplete parseState = iota
	parseDone
	parseFailed
)

// parsed is one request lifted out of a connection's byte stream.
type parsed struct {
	req       request
	n         int    // bytes of the stream this request consumed
	keepAlive bool   // false means the response must be the connection's last
	expect100 bool   // the client is waiting for a 100 Continue before sending the body
	status    int    // non-zero: answer with this instead of routing
	errBody   string // the body that goes with status
}

// parseRequest reads one request from the front of b into p.
//
// p is filled in place because parsed is 136 bytes: returning it by value cost that copy twice
// per request, once out of here and once into whatever the caller passed it on to. The caller
// owns p and reuses it, so parseRequest starts by clearing it - one memclr instead of two
// copies, and without it a field left unset by this request would still hold the last one's.
//
// parseIncomplete means more bytes are needed and nothing in p is final except expect100.
// parseFailed means the stream cannot be resynchronised: answer status and close.
func parseRequest(b []byte, p *parsed) parseState {
	*p = parsed{}

	end := bytes.Index(b, []byte("\r\n\r\n"))
	if end < 0 {
		if len(b) > maxHeadBytes {
			return p.fail(431, errHeadTooLarge)
		}
		return parseIncomplete
	}
	head := b[:end]
	rest := b[end+4:]

	line := head
	if i := bytes.Index(head, []byte("\r\n")); i >= 0 {
		line, head = head[:i], head[i+2:]
	} else {
		head = nil
	}

	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return p.fail(400, errBadRequest)
	}
	sp2 := bytes.IndexByte(line[sp1+1:], ' ')
	if sp2 < 0 {
		return p.fail(400, errBadRequest)
	}
	sp2 += sp1 + 1
	target := line[sp1+1 : sp2]
	if len(target) == 0 {
		return p.fail(400, errBadRequest)
	}
	p.req.method = line[:sp1]
	// A query string is not part of any route. Cutting it here keeps the routing table exact
	// instead of having every route tolerate a suffix.
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	p.req.path = target

	switch version := line[sp2+1:]; {
	case eq(version, "HTTP/1.1"):
		p.keepAlive = true
	case eq(version, "HTTP/1.0"):
		p.keepAlive = false
	default:
		return p.fail(505, errBadRequest)
	}

	contentLength := -1
	for len(head) > 0 {
		var h []byte
		if i := bytes.Index(head, []byte("\r\n")); i >= 0 {
			h, head = head[:i], head[i+2:]
		} else {
			h, head = head, nil
		}
		c := bytes.IndexByte(h, ':')
		if c <= 0 {
			return p.fail(400, errBadRequest)
		}
		name, value := h[:c], trimSpace(h[c+1:])
		switch {
		case eqFold(name, "authorization"):
			p.req.auth = value
		case eqFold(name, "content-length"):
			v, ok := parseContentLength(value)
			// A second, different Content-Length is a request smuggling attempt, not a typo.
			if !ok || (contentLength >= 0 && contentLength != v) {
				return p.fail(400, errBadRequest)
			}
			contentLength = v
		case eqFold(name, "transfer-encoding"):
			// The one framing this server refuses. Nothing in the harness sends it, and
			// accepting it silently would be worse than saying so.
			return p.fail(501, errUnsupported)
		case eqFold(name, "connection"):
			if tokenPresent(value, "close") {
				p.keepAlive = false
			} else if tokenPresent(value, "keep-alive") {
				p.keepAlive = true
			}
		case eqFold(name, "expect"):
			p.expect100 = tokenPresent(value, "100-continue")
		}
	}

	if contentLength < 0 {
		contentLength = 0
	}
	if contentLength > maxBodyBytes {
		return p.fail(413, errBodyTooLarge)
	}
	if len(rest) < contentLength {
		return parseIncomplete
	}
	p.req.body = rest[:contentLength]
	p.n = end + 4 + contentLength
	return parseDone
}

// fail turns p into the rejection, discarding whatever was parsed before it. keepAlive stays
// false: a request this server could not parse leaves the stream at an unknown offset, so the
// only safe answer is one response and a close.
func (p *parsed) fail(status int, body string) parseState {
	*p = parsed{status: status, errBody: body}
	return parseFailed
}

// frameResponse writes the header backwards into the reserve in front of the body and returns the
// offset the response starts at. The body is never moved.
//
// buf is a response buffer whose body occupies buf[hdrReserve : hdrReserve+bodyLen], so the
// response to send is buf[frameResponse(...) : hdrReserve+bodyLen].
func frameResponse(buf []byte, bodyLen, status int, keepAlive bool) int {
	var h [hdrReserve]byte
	p := append(h[:0], statusLine(status)...)
	p = append(p, "Content-Type: application/json\r\nContent-Length: "...)
	p = appendInt(p, int64(bodyLen))
	if !keepAlive {
		p = append(p, "\r\nConnection: close"...)
	}
	p = append(p, "\r\n\r\n"...)
	start := hdrReserve - len(p)
	copy(buf[start:hdrReserve], p)
	return start
}

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
	case 431:
		return "HTTP/1.1 431 Request Header Fields Too Large\r\n"
	case 500:
		return "HTTP/1.1 500 Internal Server Error\r\n"
	case 501:
		return "HTTP/1.1 501 Not Implemented\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	case 505:
		return "HTTP/1.1 505 HTTP Version Not Supported\r\n"
	}
	return "HTTP/1.1 500 Internal Server Error\r\n"
}

// eqFold compares a header name against a lowercase literal, ASCII case-insensitively. Header
// names are ASCII by definition, so there is no Unicode case folding to do.
func eqFold(b []byte, lower string) bool {
	if len(b) != len(lower) {
		return false
	}
	for i := range b {
		c := b[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		if c != lower[i] {
			return false
		}
	}
	return true
}

// tokenPresent reports whether a comma-separated header value contains tok.
func tokenPresent(value []byte, tok string) bool {
	for len(value) > 0 {
		t := value
		if i := bytes.IndexByte(value, ','); i >= 0 {
			t, value = value[:i], value[i+1:]
		} else {
			value = nil
		}
		if eqFold(trimSpace(t), tok) {
			return true
		}
	}
	return false
}

// parseContentLength accepts only digits: no sign, no whitespace inside, no leading plus. A
// leading zero is allowed because "0" is the common value.
func parseContentLength(b []byte) (int, bool) {
	if len(b) == 0 || len(b) > 9 {
		return 0, false
	}
	v := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int(c-'0')
	}
	return v, true
}
