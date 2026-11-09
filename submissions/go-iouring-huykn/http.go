package main

import (
	"bytes"
)

// A frame borrows its input until the response has been serialized.
type request struct {
	method, path, auth, body []byte
	hasAuth                  bool
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

// lineScanner bounds only header bytes, never the body or pipelined requests.
// LF framing remains supported for compatibility with the previous submission.
type lineScanner struct {
	wire []byte
	next int
}

func (s *lineScanner) take() ([]byte, int) {
	remaining := s.wire[s.next:]
	position := bytes.IndexByte(remaining, '\n')
	if position < 0 {
		if len(s.wire) > maxHead {
			return nil, parsedBad
		}
		return nil, parsedPartial
	}
	s.next += position + 1
	if s.next > maxHead {
		return nil, parsedBad
	}
	return bytes.TrimSuffix(remaining[:position], []byte{'\r'}), parsedDone
}

func targetLine(line []byte, dst *request) bool {
	method, rest, found := bytes.Cut(line, []byte{' '})
	if !found || len(method) == 0 {
		return false
	}
	target, protocol, found := bytes.Cut(rest, []byte{' '})
	if !found || len(target) == 0 {
		return false
	}
	if string(protocol) != "HTTP/1.1" && string(protocol) != "HTTP/1.0" {
		return false
	}
	dst.method = method
	dst.path, _, _ = bytes.Cut(target, []byte{'?'})
	return true
}

func contentBytes(value []byte) (int, bool) {
	if len(value) == 0 || len(value) > 9 {
		return 0, false
	}
	// At most nine decimal digits fit in an int even on 32-bit systems.
	length := 0
	for _, c := range value {
		if c < '0' || c > '9' {
			return 0, false
		}
		length = length*10 + int(c-'0')
	}
	return length, length <= maxBody
}

// parse separates framing from application validation. It rejects ambiguous
// length/transfer framing before handing a body to the JSON scanner.
func parse(wire []byte, dst *request) (int, int) {
	*dst = request{}
	lines := lineScanner{wire: wire}
	first, status := lines.take()
	if status != parsedDone {
		return 0, status
	}
	if !targetLine(first, dst) {
		return 0, parsedBad
	}
	bodySize, lengthSeen := 0, false
	for {
		line, status := lines.take()
		if status != parsedDone {
			return 0, status
		}
		if len(line) == 0 {
			break
		}
		name, value, found := bytes.Cut(line, []byte{':'})
		// Consumed names are at least as long as Authorization. Skip common
		// ignored headers (Host, User-Agent, Content-Type) before value work.
		if !found || len(name) < len("Authorization") {
			continue
		}
		switch {
		case bytes.EqualFold(name, []byte("Authorization")):
			dst.auth, dst.hasAuth = bytes.Trim(value, " \t"), true
		case bytes.EqualFold(name, []byte("Content-Length")):
			if lengthSeen {
				return 0, parsedBad
			}
			lengthSeen = true
			var valid bool
			bodySize, valid = contentBytes(bytes.Trim(value, " \t"))
			if !valid {
				return 0, parsedBad
			}
		case bytes.EqualFold(name, []byte("Transfer-Encoding")):
			return 0, parsedBad
		}
	}
	end := lines.next + bodySize
	if end > len(wire) {
		return 0, parsedPartial
	}
	dst.body = wire[lines.next:end]
	return end, parsedDone
}
