package main

import "bytes"

const (
	maxHeaderBytes = 16 << 10
	maxBodyBytes   = 64 << 10
)

type parseStatus int

const (
	parseIncomplete parseStatus = iota
	parseOK
	parseBad
	parseTooLarge
	parseUnsupported
)

// request points into the connection's input bytes; it is valid until the next read.
type request struct {
	method, path, body []byte
	auth               []byte
	hasAuth            bool
	keepAlive          bool
	http10             bool
	expectContinue     bool
	size               int
}

var (
	crlfcrlf = []byte("\r\n\r\n")
	http11   = []byte("HTTP/1.1")
	http10   = []byte("HTTP/1.0")
)

// parseRequest reads one HTTP/1.x request from the front of b. With parseIncomplete it still
// fills r.expectContinue once the head is complete, so the caller can answer an Expect header.
func parseRequest(b []byte, r *request) parseStatus {
	end := bytes.Index(b, crlfcrlf)
	if end < 0 {
		if len(b) > maxHeaderBytes {
			return parseTooLarge
		}
		return parseIncomplete
	}
	head := b[:end]
	le := bytes.IndexByte(head, '\n')
	if le < 0 {
		le = len(head)
	}
	line := head[:le]
	if len(line) > 0 && line[len(line)-1] == '\r' {
		line = line[:len(line)-1]
	}
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return parseBad
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 {
		return parseBad
	}
	r.method = line[:sp1]
	target := rest[:sp2]
	version := rest[sp2+1:]
	switch {
	case bytes.Equal(version, http11):
		r.keepAlive, r.http10 = true, false
	case bytes.Equal(version, http10):
		r.keepAlive, r.http10 = false, true
	default:
		return parseBad
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.path = target
	r.hasAuth, r.expectContinue, r.auth = false, false, nil

	contentLength := 0
	for h := head[min(le+1, len(head)):]; len(h) > 0; {
		nl := bytes.IndexByte(h, '\n')
		var hl []byte
		if nl < 0 {
			hl, h = h, nil
		} else {
			hl, h = h[:nl], h[nl+1:]
		}
		if len(hl) > 0 && hl[len(hl)-1] == '\r' {
			hl = hl[:len(hl)-1]
		}
		colon := bytes.IndexByte(hl, ':')
		if colon <= 0 {
			return parseBad
		}
		name, val := hl[:colon], trimOWS(hl[colon+1:])
		switch len(name) {
		case 14:
			if asciiEqualFold(name, "content-length") {
				n, ok := parseContentLength(val)
				if !ok {
					return parseBad
				}
				contentLength = n
			}
		case 13:
			if asciiEqualFold(name, "authorization") {
				r.auth, r.hasAuth = val, true
			}
		case 10:
			if asciiEqualFold(name, "connection") {
				if asciiEqualFold(val, "close") {
					r.keepAlive = false
				} else if asciiEqualFold(val, "keep-alive") {
					r.keepAlive = true
				}
			}
		case 17:
			if asciiEqualFold(name, "transfer-encoding") {
				return parseUnsupported
			}
		case 6:
			if asciiEqualFold(name, "expect") && asciiEqualFold(val, "100-continue") {
				r.expectContinue = true
			}
		}
	}
	if contentLength > maxBodyBytes {
		return parseTooLarge
	}
	total := end + 4 + contentLength
	if len(b) < total {
		return parseIncomplete
	}
	r.body = b[end+4 : total]
	r.size = total
	return parseOK
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

func lower(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c + 32
	}
	return c
}

func asciiEqualFold(b []byte, lowerName string) bool {
	if len(b) != len(lowerName) {
		return false
	}
	for i := range b {
		if lower(b[i]) != lowerName[i] {
			return false
		}
	}
	return true
}

func parseContentLength(b []byte) (int, bool) {
	if len(b) == 0 {
		return 0, false
	}
	n := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		if n <= maxBodyBytes {
			n = n*10 + int(c-'0')
		}
	}
	return n, true
}
