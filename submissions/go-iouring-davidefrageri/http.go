package main

import "bytes"

// request is one framed HTTP/1.1 request; its slices point into the receive buffer and are
// valid only until the next parse.
type request struct {
	method     byte
	path       []byte
	auth       []byte
	body       []byte
	hasAuth    bool
	closeAfter bool
}

const (
	mGET = iota + 1
	mPOST
	mOther
)

const (
	maxHead = 16 << 10
	maxBody = 1 << 20
)

const (
	parsedDone = iota
	parsedPartial
	parsedBad
)

var crlfcrlf = []byte("\r\n\r\n")

func trimSP(s []byte) []byte {
	for len(s) > 0 && (s[0] == ' ' || s[0] == '\t') {
		s = s[1:]
	}
	for len(s) > 0 && (s[len(s)-1] == ' ' || s[len(s)-1] == '\t') {
		s = s[:len(s)-1]
	}
	return s
}

// lowerEq reports whether a equals the lower-case ASCII name, ignoring case.
func lowerEq(a []byte, name string) bool {
	if len(a) != len(name) {
		return false
	}
	for i := 0; i < len(a); i++ {
		if a[i]|0x20 != name[i] {
			return false
		}
	}
	return true
}

// parse frames one request at the start of buf and returns the bytes it used. Only the request
// line, Authorization, Content-Length, Transfer-Encoding and Connection are looked at.
func parse(buf []byte, r *request) (int, int) {
	end := bytes.Index(buf, crlfcrlf)
	if end < 0 {
		if len(buf) > maxHead {
			return 0, parsedBad
		}
		return 0, parsedPartial
	}
	if end > maxHead {
		return 0, parsedBad
	}
	head := buf[:end]
	nl := bytes.IndexByte(head, '\n')
	line := head
	if nl >= 0 {
		line = head[:nl]
	}
	if len(line) > 0 && line[len(line)-1] == '\r' {
		line = line[:len(line)-1]
	}
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return 0, parsedBad
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 || len(rest)-sp2-1 != 8 || string(rest[sp2+1:sp2+8]) != "HTTP/1." {
		return 0, parsedBad
	}
	switch {
	case sp1 == 3 && string(line[:3]) == "GET":
		r.method = mGET
	case sp1 == 4 && string(line[:4]) == "POST":
		r.method = mPOST
	default:
		r.method = mOther
	}
	target := rest[:sp2]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.path = target
	r.auth = nil
	r.hasAuth = false
	r.closeAfter = rest[sp2+8] == '0' // HTTP/1.0: no keep-alive unless asked
	clen := 0
	pos := nl + 1
	if nl < 0 {
		pos = len(head)
	}
	for pos < len(head) {
		n := bytes.IndexByte(head[pos:], '\n')
		var l []byte
		if n < 0 {
			l = head[pos:]
			pos = len(head)
		} else {
			l = head[pos : pos+n]
			pos += n + 1
		}
		if len(l) > 0 && l[len(l)-1] == '\r' {
			l = l[:len(l)-1]
		}
		if len(l) == 0 {
			continue
		}
		switch l[0] | 0x20 {
		case 'a', 'c', 't':
		default:
			continue
		}
		colon := bytes.IndexByte(l, ':')
		if colon < 0 {
			continue
		}
		name := l[:colon]
		switch len(name) {
		case 13:
			if lowerEq(name, "authorization") {
				r.auth = trimSP(l[colon+1:])
				r.hasAuth = true
			}
		case 14:
			if lowerEq(name, "content-length") {
				v := trimSP(l[colon+1:])
				if len(v) == 0 || len(v) > 9 {
					return 0, parsedBad
				}
				clen = 0
				for _, c := range v {
					if c < '0' || c > '9' {
						return 0, parsedBad
					}
					clen = clen*10 + int(c-'0')
				}
				if clen > maxBody {
					return 0, parsedBad
				}
			}
		case 10:
			if lowerEq(name, "connection") {
				v := trimSP(l[colon+1:])
				if lowerEq(v, "close") {
					r.closeAfter = true
				} else if lowerEq(v, "keep-alive") {
					r.closeAfter = false
				}
			}
		case 17:
			if lowerEq(name, "transfer-encoding") {
				return 0, parsedBad
			}
		}
	}
	pos = end + 4
	if len(buf) < pos+clen {
		return 0, parsedPartial
	}
	r.body = buf[pos : pos+clen]
	return pos + clen, parsedDone
}

// ---- responses built in Go (errors and the like) ---------------------------------------------------

// start writes the status line and headers; Content-Length is a 5-character field, right-aligned
// with leading spaces (optional whitespace before a field value, RFC 9110 5.6.3), filled by end.
func start(b []byte, status int) ([]byte, int) {
	switch status {
	case 200:
		b = append(b, "HTTP/1.1 200 OK\r\n"...)
	case 201:
		b = append(b, "HTTP/1.1 201 Created\r\n"...)
	case 400:
		b = append(b, "HTTP/1.1 400 Bad Request\r\n"...)
	case 401:
		b = append(b, "HTTP/1.1 401 Unauthorized\r\n"...)
	case 404:
		b = append(b, "HTTP/1.1 404 Not Found\r\n"...)
	case 503:
		b = append(b, "HTTP/1.1 503 Service Unavailable\r\n"...)
	default:
		b = append(b, "HTTP/1.1 500 Internal Server Error\r\n"...)
	}
	b = append(b, "Content-Type: application/json\r\nContent-Length:     0\r\n\r\n"...)
	return b, len(b)
}

// end fills in Content-Length for the body that starts at body.
func end(b []byte, body int) []byte {
	n := len(b) - body
	f := b[body-9 : body-4]
	for i := 4; i >= 0; i-- {
		if n == 0 && i < 4 {
			f[i] = ' '
			continue
		}
		f[i] = byte('0' + n%10)
		n /= 10
	}
	return b
}

func errorResp(b []byte, status int, msg string) []byte {
	b, s := start(b, status)
	b = append(b, `{"error":"`...)
	b = append(b, msg...)
	b = append(b, `"}`...)
	return end(b, s)
}

// positive parses a positive decimal integer without sign or leading zeros (below 2^53).
func positive(s []byte) (int64, bool) {
	if len(s) == 0 || len(s) > 16 || s[0] == '0' {
		return 0, false
	}
	var v int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, v <= 1<<53
}
