package main

import (
	"bytes"
	"unicode/utf8"
)

const (
	byteMissing = iota
	byteString
	byteNumber
	byteOther
)

// Values refer to the original input; no object, map or decoded document is built.
type byteValue struct {
	raw     []byte
	kind    uint8
	escaped bool
}

type byteReader struct {
	s   []byte
	pos int
}

// walkObject follows kensac's parse_object: validate the entire UTF-8 document,
// visit top-level fields in order, and validate/skip nested values. Last duplicates win.
func walkObject(s, scratch []byte, visit func([]byte, byteValue)) (valid, object bool) {
	if !utf8.Valid(s) {
		return false, false
	}
	r := byteReader{s: s}
	r.space()
	object = r.pos < len(s) && s[r.pos] == '{'
	if object {
		valid = r.object(0, scratch, visit)
	} else {
		valid = r.value(0).kind != byteMissing
	}
	r.space()
	return valid && r.pos == len(s), object
}

func (r *byteReader) space() {
	for r.pos < len(r.s) {
		switch r.s[r.pos] {
		case ' ', '\t', '\n', '\r':
			r.pos++
		default:
			return
		}
	}
}

func (r *byteReader) take(c byte) bool {
	if r.pos == len(r.s) || r.s[r.pos] != c {
		return false
	}
	r.pos++
	return true
}

func (r *byteReader) text() byteValue {
	if !r.take('"') {
		return byteValue{}
	}
	start, escaped := r.pos, false
	for r.pos < len(r.s) {
		c := r.s[r.pos]
		r.pos++
		switch {
		case c == '"':
			return byteValue{raw: r.s[start : r.pos-1], kind: byteString, escaped: escaped}
		case c == '\\':
			escaped = true
			if r.pos == len(r.s) {
				return byteValue{}
			}
			e := r.s[r.pos]
			r.pos++
			switch e {
			case '"', '\\', '/', 'b', 'f', 'n', 'r', 't':
			case 'u':
				if len(r.s)-r.pos < 4 {
					return byteValue{}
				}
				for _, h := range r.s[r.pos : r.pos+4] {
					if unhex(h) < 0 {
						return byteValue{}
					}
				}
				r.pos += 4
			default:
				return byteValue{}
			}
		case c < 0x20:
			return byteValue{}
		}
	}
	return byteValue{}
}

func (r *byteReader) digits() bool {
	start := r.pos
	for r.pos < len(r.s) && r.s[r.pos] >= '0' && r.s[r.pos] <= '9' {
		r.pos++
	}
	return r.pos != start
}

func (r *byteReader) number() byteValue {
	start := r.pos
	r.take('-')
	if !r.take('0') {
		if r.pos == len(r.s) || r.s[r.pos] < '1' || r.s[r.pos] > '9' || !r.digits() {
			return byteValue{}
		}
	}
	if r.take('.') && !r.digits() {
		return byteValue{}
	}
	if r.take('e') || r.take('E') {
		if !r.take('+') {
			r.take('-')
		}
		if !r.digits() {
			return byteValue{}
		}
	}
	return byteValue{raw: r.s[start:r.pos], kind: byteNumber}
}

func (r *byteReader) value(depth int) byteValue {
	r.space()
	if depth > 64 || r.pos == len(r.s) {
		return byteValue{}
	}
	switch r.s[r.pos] {
	case '"':
		return r.text()
	case '{':
		if !r.object(depth, nil, nil) {
			return byteValue{}
		}
	case '[':
		r.pos++
		r.space()
		if !r.take(']') {
			for {
				if r.value(depth+1).kind == byteMissing {
					return byteValue{}
				}
				r.space()
				if r.take(']') {
					break
				}
				if !r.take(',') {
					return byteValue{}
				}
			}
		}
	case 't', 'f', 'n':
		word := "null"
		if r.s[r.pos] == 't' {
			word = "true"
		} else if r.s[r.pos] == 'f' {
			word = "false"
		}
		if !bytes.HasPrefix(r.s[r.pos:], []byte(word)) {
			return byteValue{}
		}
		r.pos += len(word)
	default:
		return r.number()
	}
	return byteValue{kind: byteOther}
}

func (r *byteReader) object(depth int, scratch []byte, visit func([]byte, byteValue)) bool {
	r.pos++ // '{'
	r.space()
	if r.take('}') {
		return true
	}
	for {
		r.space()
		key := r.text()
		if key.kind != byteString {
			return false
		}
		r.space()
		if !r.take(':') {
			return false
		}
		value := r.value(depth + 1)
		if value.kind == byteMissing {
			return false
		}
		if visit != nil {
			name := key.raw
			if key.escaped {
				name = unescape(name, scratch[:0])
			}
			visit(name, value)
		}
		r.space()
		if r.take('}') {
			return true
		}
		if !r.take(',') {
			return false
		}
	}
}

func unhex(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

func hex4(s []byte) int {
	return unhex(s[0])<<12 | unhex(s[1])<<8 | unhex(s[2])<<4 | unhex(s[3])
}

// Decode a validated string in runs; lone surrogates become U+FFFD, as in kensac.
func unescape(s, dst []byte) []byte {
	for len(s) > 0 {
		i := bytes.IndexByte(s, '\\')
		if i < 0 {
			return append(dst, s...)
		}
		dst = append(dst, s[:i]...)
		s = s[i+1:]
		c := s[0]
		s = s[1:]
		switch c {
		case 'b':
			dst = append(dst, '\b')
		case 'f':
			dst = append(dst, '\f')
		case 'n':
			dst = append(dst, '\n')
		case 'r':
			dst = append(dst, '\r')
		case 't':
			dst = append(dst, '\t')
		case 'u':
			r := rune(hex4(s))
			s = s[4:]
			if r >= 0xd800 && r < 0xdc00 && len(s) >= 6 && s[0] == '\\' && s[1] == 'u' {
				lo := rune(hex4(s[2:]))
				if lo >= 0xdc00 && lo < 0xe000 {
					r = 0x10000 + (r-0xd800)<<10 + lo - 0xdc00
					s = s[6:]
				}
			}
			if r >= 0xd800 && r < 0xe000 {
				r = utf8.RuneError
			}
			dst = utf8.AppendRune(dst, r)
		default:
			dst = append(dst, c)
		}
	}
	return dst
}
