package main

import (
	"bytes"
	"strconv"
	"unicode/utf8"
)

const maxDepth = 256

func isWS(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' }

func skipWS(b []byte, i int) int {
	for i < len(b) && isWS(b[i]) {
		i++
	}
	return i
}

func validJSON(b []byte) bool {
	i, ok := scanValue(b, skipWS(b, 0), 0)
	return ok && skipWS(b, i) == len(b)
}

func scanValue(b []byte, i, depth int) (int, bool) {
	if i >= len(b) || depth > maxDepth {
		return i, false
	}
	switch c := b[i]; {
	case c == '{':
		i = skipWS(b, i+1)
		if i < len(b) && b[i] == '}' {
			return i + 1, true
		}
		for {
			if i >= len(b) || b[i] != '"' {
				return i, false
			}
			var ok bool
			if i, ok = scanString(b, i); !ok {
				return i, false
			}
			i = skipWS(b, i)
			if i >= len(b) || b[i] != ':' {
				return i, false
			}
			if i, ok = scanValue(b, skipWS(b, i+1), depth+1); !ok {
				return i, false
			}
			i = skipWS(b, i)
			if i >= len(b) {
				return i, false
			}
			if b[i] == '}' {
				return i + 1, true
			}
			if b[i] != ',' {
				return i, false
			}
			i = skipWS(b, i+1)
		}
	case c == '[':
		i = skipWS(b, i+1)
		if i < len(b) && b[i] == ']' {
			return i + 1, true
		}
		for {
			var ok bool
			if i, ok = scanValue(b, i, depth+1); !ok {
				return i, false
			}
			i = skipWS(b, i)
			if i >= len(b) {
				return i, false
			}
			if b[i] == ']' {
				return i + 1, true
			}
			if b[i] != ',' {
				return i, false
			}
			i = skipWS(b, i+1)
		}
	case c == '"':
		return scanString(b, i)
	case c == '-' || (c >= '0' && c <= '9'):
		return scanNumber(b, i)
	case c == 't':
		return lit(b, i, "true")
	case c == 'f':
		return lit(b, i, "false")
	case c == 'n':
		return lit(b, i, "null")
	}
	return i, false
}

func lit(b []byte, i int, s string) (int, bool) {
	if len(b)-i >= len(s) && string(b[i:i+len(s)]) == s {
		return i + len(s), true
	}
	return i, false
}

func isDigit(c byte) bool { return c >= '0' && c <= '9' }

func scanNumber(b []byte, i int) (int, bool) {
	if b[i] == '-' {
		i++
	}
	switch {
	case i < len(b) && b[i] == '0':
		i++
	case i < len(b) && b[i] >= '1' && b[i] <= '9':
		for i < len(b) && isDigit(b[i]) {
			i++
		}
	default:
		return i, false
	}
	if i < len(b) && b[i] == '.' {
		i++
		if i >= len(b) || !isDigit(b[i]) {
			return i, false
		}
		for i < len(b) && isDigit(b[i]) {
			i++
		}
	}
	if i < len(b) && (b[i] == 'e' || b[i] == 'E') {
		i++
		if i < len(b) && (b[i] == '+' || b[i] == '-') {
			i++
		}
		if i >= len(b) || !isDigit(b[i]) {
			return i, false
		}
		for i < len(b) && isDigit(b[i]) {
			i++
		}
	}
	return i, true
}

func hexVal(c byte) int {
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

func scanString(b []byte, i int) (int, bool) {
	i++
	for i < len(b) {
		c := b[i]
		if c >= 0x20 && c < 0x80 && c != '"' && c != '\\' {
			i++
			continue
		}
		switch {
		case c == '"':
			return i + 1, true
		case c == '\\':
			if i+1 >= len(b) {
				return i, false
			}
			switch b[i+1] {
			case '"', '\\', '/', 'b', 'f', 'n', 'r', 't':
				i += 2
			case 'u':
				if i+6 > len(b) || hexVal(b[i+2]) < 0 || hexVal(b[i+3]) < 0 || hexVal(b[i+4]) < 0 || hexVal(b[i+5]) < 0 {
					return i, false
				}
				i += 6
			default:
				return i, false
			}
		case c < 0x20:
			return i, false
		default:
			r, n := utf8.DecodeRune(b[i:])
			if r == utf8.RuneError && n <= 1 {
				return i, false
			}
			i += n
		}
	}
	return i, false
}

func unescapeRef(dst, s []byte) []byte {
	inner := s[1 : len(s)-1]
	if bytes.IndexByte(inner, '\\') < 0 {
		return inner
	}
	return unescape(dst, s)
}

func unescape(dst, s []byte) []byte {
	s = s[1 : len(s)-1]
	for i := 0; i < len(s); {
		if s[i] != '\\' {
			j := bytes.IndexByte(s[i:], '\\')
			if j < 0 {
				dst = append(dst, s[i:]...)
				break
			}
			dst = append(dst, s[i:i+j]...)
			i += j
			continue
		}
		switch s[i+1] {
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
			r := rune(hex4(s[i+2:]))
			i += 6
			if r >= 0xD800 && r < 0xDC00 && i+6 <= len(s) && s[i] == '\\' && s[i+1] == 'u' {
				if lo := rune(hex4(s[i+2:])); lo >= 0xDC00 && lo < 0xE000 {
					r = 0x10000 + (r-0xD800)<<10 + (lo - 0xDC00)
					i += 6
				}
			}
			dst = utf8.AppendRune(dst, r)
			continue
		default:
			dst = append(dst, s[i+1])
		}
		i += 2
	}
	return dst
}

func hex4(b []byte) int {
	return hexVal(b[0])<<12 | hexVal(b[1])<<8 | hexVal(b[2])<<4 | hexVal(b[3])
}

type member struct {
	key, val []byte
}

type objectWalker struct {
	b    []byte
	i    int
	done bool
	err  bool
}

func newObjectWalker(b []byte) (objectWalker, bool) {
	i := skipWS(b, 0)
	if i >= len(b) || b[i] != '{' {
		return objectWalker{b: b, err: true}, false
	}
	i = skipWS(b, i+1)
	if i < len(b) && b[i] == '}' {
		if skipWS(b, i+1) != len(b) {
			return objectWalker{b: b, err: true}, false
		}
		return objectWalker{b: b, i: len(b), done: true}, true
	}
	return objectWalker{b: b, i: i}, true
}

func (w *objectWalker) next() (member, bool) {
	if w.done || w.err {
		return member{}, false
	}
	b := w.b
	i := w.i
	if i >= len(b) || b[i] != '"' {
		w.err = true
		return member{}, false
	}
	ke, ok := scanString(b, i)
	if !ok {
		w.err = true
		return member{}, false
	}
	vs := skipWS(b, ke)
	if vs >= len(b) || b[vs] != ':' {
		w.err = true
		return member{}, false
	}
	vs = skipWS(b, vs+1)
	ve, ok := scanValue(b, vs, 1)
	if !ok {
		w.err = true
		return member{}, false
	}
	after := skipWS(b, ve)
	if after >= len(b) {
		w.err = true
		return member{}, false
	}
	if b[after] == '}' {
		if skipWS(b, after+1) != len(b) {
			w.err = true
			return member{}, false
		}
		w.i = len(b)
		w.done = true
	} else if b[after] == ',' {
		w.i = skipWS(b, after+1)
	} else {
		w.err = true
		return member{}, false
	}
	return member{key: b[i:ke], val: b[vs:ve]}, true
}

func keyIs(raw []byte, name string, scratch []byte) bool {
	inner := raw[1 : len(raw)-1]
	if string(inner) == name {
		return true
	}
	if bytes.IndexByte(inner, '\\') >= 0 {
		return string(unescape(scratch[:0], raw)) == name
	}
	return false
}

func parseJSONNumber(v []byte) (float64, bool) {
	if len(v) == 0 || (v[0] != '-' && !isDigit(v[0])) {
		return 0, false
	}
	// Fast path for plain positive integer timestamps (iat/exp)
	if len(v) <= 18 {
		var n int64
		allDigits := true
		for _, c := range v {
			if c < '0' || c > '9' {
				allDigits = false
				break
			}
			n = n*10 + int64(c-'0')
		}
		if allDigits {
			return float64(n), true
		}
	}
	f, err := strconv.ParseFloat(string(v), 64)
	return f, err == nil
}

func isTrimSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xA0, 0x1680, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
		return true
	}
	return r >= 0x2000 && r <= 0x200A
}

func trimSpace(s []byte) []byte {
	for len(s) > 0 {
		if s[0] < 0x80 {
			if !isTrimSpace(rune(s[0])) {
				break
			}
			s = s[1:]
			continue
		}
		r, n := utf8.DecodeRune(s)
		if !isTrimSpace(r) {
			break
		}
		s = s[n:]
	}
	for len(s) > 0 {
		last := s[len(s)-1]
		if last < 0x80 {
			if !isTrimSpace(rune(last)) {
				break
			}
			s = s[:len(s)-1]
			continue
		}
		r, n := utf8.DecodeLastRune(s)
		if !isTrimSpace(r) {
			break
		}
		s = s[:len(s)-n]
	}
	return s
}
