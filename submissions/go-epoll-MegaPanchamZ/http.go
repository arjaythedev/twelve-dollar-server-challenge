package main

import (
	"bytes"
)

const (
	maxHeader = 16 << 10
	maxBody   = 16 << 10
)

var (
	methodGet  = "GET"
	methodPost = "POST"
)

type request struct {
	method, path, auth, body []byte
	hasAuth                  bool
	close                    bool
	expectContinue           bool
}

func eqFoldASCII(b []byte, s string) bool {
	if string(b) == s {
		return true
	}
	if len(b) != len(s) {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := b[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		sc := s[i]
		if sc >= 'A' && sc <= 'Z' {
			sc += 'a' - 'A'
		}
		if c != sc {
			return false
		}
	}
	return true
}

// parseRequest parses one HTTP/1.x request from b.
// Returns bytes consumed (>0), 0 if incomplete, or -1 if malformed/unsupported.
func parseRequest(b []byte, r *request) int {
	r.expectContinue = false
	nl := bytes.IndexByte(b, '\n')
	if nl < 0 {
		if len(b) > maxHeader {
			return -1
		}
		return 0
	}
	if nl == 0 || b[nl-1] != '\r' {
		return -1
	}
	line := b[:nl-1]
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return -1
	}
	method := line[:sp1]
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 {
		return -1
	}
	target := rest[:sp2]
	proto := rest[sp2+1:]
	if target[0] != '/' || len(proto) != 8 || string(proto[:7]) != "HTTP/1." || (proto[7] != '1' && proto[7] != '0') {
		return -1
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.method = method
	r.path = target
	r.auth = nil
	r.body = nil
	r.hasAuth = false
	r.close = proto[7] == '0'

	length := 0
	seenCL := false
	pos := nl + 1
	for {
		idx := bytes.IndexByte(b[pos:], '\n')
		if idx < 0 {
			if len(b) > maxHeader {
				return -1
			}
			return 0
		}
		if idx == 0 || b[pos+idx-1] != '\r' {
			return -1
		}
		if idx == 1 {
			if pos-2 > maxHeader {
				return -1
			}
			pos += 2
			break
		}
		h := b[pos : pos+idx-1]
		pos += idx + 1
		if pos > maxHeader+2 {
			return -1
		}
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return -1
		}
		name := h[:colon]
		value := trimASCII(h[colon+1:])
		switch len(name) {
		case 14: // Content-Length
			if eqFoldASCII(name, "Content-Length") {
				v, ok := parseContentLen(value)
				if !ok || v > maxBody || (seenCL && v != length) {
					return -1
				}
				length = v
				seenCL = true
			}
		case 13: // Authorization
			if eqFoldASCII(name, "Authorization") {
				r.auth = value
				r.hasAuth = true
			}
		case 10: // Connection
			if eqFoldASCII(name, "Connection") {
				if eqFoldASCII(value, "close") {
					r.close = true
				} else if eqFoldASCII(value, "keep-alive") {
					r.close = false
				}
			}
		case 17: // Transfer-Encoding
			if eqFoldASCII(name, "Transfer-Encoding") {
				return -1
			}
		case 6: // Expect
			if eqFoldASCII(name, "Expect") && eqFoldASCII(value, "100-continue") {
				r.expectContinue = true
			}
		}
	}
	total := pos + length
	if len(b) < total {
		return 0
	}
	r.body = b[pos:total]
	return total
}

func trimASCII(b []byte) []byte {
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t') {
		b = b[1:]
	}
	for len(b) > 0 && (b[len(b)-1] == ' ' || b[len(b)-1] == '\t') {
		b = b[:len(b)-1]
	}
	return b
}

func parseContentLen(b []byte) (int, bool) {
	if len(b) == 0 || len(b) > 9 {
		return 0, false
	}
	n := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
	}
	return n, true
}
