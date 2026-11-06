package main

import "bytes"

// request is one parsed HTTP/1.1 request; its slices point into the receive buffer.
type request struct {
	method []byte
	path   []byte
	auth   []byte
	hasAuth bool
	body   []byte
}

const (
	maxHead = 16 << 10
	maxBody = 1 << 20
)

const (
	parsedDone = iota
	parsedPartial
	parsedBad
)

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

// parse frames one request at the start of buf. It returns the bytes used when done.
// Only the request line, Authorization, Content-Length and Transfer-Encoding are looked at.
func parse(buf []byte, r *request) (int, int) {
	partial := parsedPartial
	if len(buf) > maxHead {
		partial = parsedBad
	}
	nl := bytes.IndexByte(buf, '\n')
	if nl < 0 {
		return 0, partial
	}
	line := buf[:nl]
	if len(line) > 0 && line[len(line)-1] == '\r' {
		line = line[:len(line)-1]
	}
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return 0, parsedBad
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 || !bytes.HasPrefix(rest[sp2+1:], []byte("HTTP/1.")) {
		return 0, parsedBad
	}
	r.method = line[:sp1]
	target := rest[:sp2]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.path = target
	r.auth = nil
	r.hasAuth = false
	clen := 0
	pos := nl + 1
	for {
		n := bytes.IndexByte(buf[pos:], '\n')
		if n < 0 {
			return 0, partial
		}
		l := buf[pos : pos+n]
		pos += n + 1
		if len(l) > 0 && l[len(l)-1] == '\r' {
			l = l[:len(l)-1]
		}
		if len(l) == 0 {
			break
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
		case 17:
			if lowerEq(name, "transfer-encoding") {
				return 0, parsedBad
			}
		}
	}
	if len(buf) < pos+clen {
		return 0, parsedPartial
	}
	r.body = buf[pos : pos+clen]
	return pos + clen, parsedDone
}
