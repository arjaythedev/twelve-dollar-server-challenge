package main

import (
	"bytes"
	"strconv"
	"unicode/utf8"
	"unsafe"
)

// Key/value constants for byte comparisons (avoids []byte(string) allocations).
var (
	keyAlg      = []byte("alg")
	keySub      = []byte("sub")
	keyUsername = []byte("username")
	keyExp      = []byte("exp")
	keyNbf      = []byte("nbf")
	keyBody     = []byte("body")
	valHS256    = []byte("HS256")
)

func unsafeString(b []byte) string {
	if len(b) == 0 {
		return ""
	}
	return unsafe.String(&b[0], len(b))
}

// jlex is a minimal, allocation-free JSON scanner for the flat objects this API
// handles: the JWT header and payload, and {"body": ...}. Strings are unescaped
// in place, so values returned by str() alias the input buffer.
type jlex struct {
	b []byte
	i int
}

func (l *jlex) ws() {
	for l.i < len(l.b) {
		switch l.b[l.i] {
		case ' ', '\t', '\n', '\r':
			l.i++
		default:
			return
		}
	}
}

// hex4 consumes "uXXXX" (the caller has already consumed the backslash) and
// returns the code unit.
func (l *jlex) hex4() (rune, bool) {
	if l.i >= len(l.b) || l.b[l.i] != 'u' || l.i+4 >= len(l.b) {
		return 0, false
	}
	var r rune
	for k := 1; k <= 4; k++ {
		c := l.b[l.i+k]
		var d rune
		switch {
		case c >= '0' && c <= '9':
			d = rune(c - '0')
		case c >= 'a' && c <= 'f':
			d = rune(c-'a') + 10
		case c >= 'A' && c <= 'F':
			d = rune(c-'A') + 10
		default:
			return 0, false
		}
		r = r*16 + d
	}
	l.i += 5
	return r, true
}

// str parses a JSON string, unescaping it in place, and returns the value.
func (l *jlex) str() ([]byte, bool) {
	if l.i >= len(l.b) || l.b[l.i] != '"' {
		return nil, false
	}
	l.i++
	start := l.i
	w := l.i
	for l.i < len(l.b) {
		c := l.b[l.i]
		switch {
		case c == '"':
			l.i++
			return l.b[start:w], true
		case c == '\\':
			l.i++
			if l.i >= len(l.b) {
				return nil, false
			}
			switch e := l.b[l.i]; e {
			case '"', '\\', '/':
				l.b[w] = e
				w++
				l.i++
			case 'b':
				l.b[w] = '\b'
				w++
				l.i++
			case 'f':
				l.b[w] = '\f'
				w++
				l.i++
			case 'n':
				l.b[w] = '\n'
				w++
				l.i++
			case 'r':
				l.b[w] = '\r'
				w++
				l.i++
			case 't':
				l.b[w] = '\t'
				w++
				l.i++
			case 'u':
				r, ok := l.hex4()
				if !ok {
					return nil, false
				}
				if r >= 0xD800 && r <= 0xDBFF && l.i+1 < len(l.b) && l.b[l.i] == '\\' && l.b[l.i+1] == 'u' {
					l.i++
					r2, ok2 := l.hex4()
					if ok2 && r2 >= 0xDC00 && r2 <= 0xDFFF {
						r = 0x10000 + (r-0xD800)<<10 + (r2 - 0xDC00)
					} else {
						r = utf8.RuneError
					}
				} else if r >= 0xD800 && r <= 0xDFFF {
					r = utf8.RuneError
				}
				w += utf8.EncodeRune(l.b[w:], r)
			default:
				return nil, false
			}
		default:
			l.b[w] = c
			w++
			l.i++
		}
	}
	return nil, false
}

// num parses a JSON number.
func (l *jlex) num() (float64, bool) {
	start := l.i
	if l.i < len(l.b) && l.b[l.i] == '-' {
		l.i++
	}
	d0 := l.i
	for l.i < len(l.b) && l.b[l.i] >= '0' && l.b[l.i] <= '9' {
		l.i++
	}
	if l.i == d0 {
		return 0, false
	}
	if l.i < len(l.b) && l.b[l.i] == '.' {
		l.i++
		d1 := l.i
		for l.i < len(l.b) && l.b[l.i] >= '0' && l.b[l.i] <= '9' {
			l.i++
		}
		if l.i == d1 {
			return 0, false
		}
	}
	if l.i < len(l.b) && (l.b[l.i] == 'e' || l.b[l.i] == 'E') {
		l.i++
		if l.i < len(l.b) && (l.b[l.i] == '+' || l.b[l.i] == '-') {
			l.i++
		}
		d2 := l.i
		for l.i < len(l.b) && l.b[l.i] >= '0' && l.b[l.i] <= '9' {
			l.i++
		}
		if l.i == d2 {
			return 0, false
		}
	}
	f, err := strconv.ParseFloat(unsafeString(l.b[start:l.i]), 64)
	if err != nil {
		return 0, false
	}
	return f, true
}

// skipValue consumes any JSON value.
func (l *jlex) skipValue() bool {
	l.ws()
	if l.i >= len(l.b) {
		return false
	}
	switch c := l.b[l.i]; {
	case c == '"':
		_, ok := l.str()
		return ok
	case c == '{':
		l.i++
		l.ws()
		if l.i < len(l.b) && l.b[l.i] == '}' {
			l.i++
			return true
		}
		for {
			l.ws()
			if _, ok := l.str(); !ok {
				return false
			}
			l.ws()
			if l.i >= len(l.b) || l.b[l.i] != ':' {
				return false
			}
			l.i++
			if !l.skipValue() {
				return false
			}
			l.ws()
			if l.i >= len(l.b) {
				return false
			}
			if l.b[l.i] == ',' {
				l.i++
				continue
			}
			if l.b[l.i] == '}' {
				l.i++
				return true
			}
			return false
		}
	case c == '[':
		l.i++
		l.ws()
		if l.i < len(l.b) && l.b[l.i] == ']' {
			l.i++
			return true
		}
		for {
			if !l.skipValue() {
				return false
			}
			l.ws()
			if l.i >= len(l.b) {
				return false
			}
			if l.b[l.i] == ',' {
				l.i++
				continue
			}
			if l.b[l.i] == ']' {
				l.i++
				return true
			}
			return false
		}
	case c == 't':
		if l.i+4 <= len(l.b) && string(l.b[l.i:l.i+4]) == "true" {
			l.i += 4
			return true
		}
		return false
	case c == 'f':
		if l.i+5 <= len(l.b) && string(l.b[l.i:l.i+5]) == "false" {
			l.i += 5
			return true
		}
		return false
	case c == 'n':
		if l.i+4 <= len(l.b) && string(l.b[l.i:l.i+4]) == "null" {
			l.i += 4
			return true
		}
		return false
	default:
		_, ok := l.num()
		return ok
	}
}

// headerAlg returns the "alg" value of a JWT header object.
func headerAlg(hb []byte) ([]byte, bool) {
	l := jlex{b: hb}
	l.ws()
	if l.i >= len(l.b) || l.b[l.i] != '{' {
		return nil, false
	}
	l.i++
	l.ws()
	if l.i < len(l.b) && l.b[l.i] == '}' {
		return nil, false
	}
	for {
		l.ws()
		k, ok := l.str()
		if !ok {
			return nil, false
		}
		l.ws()
		if l.i >= len(l.b) || l.b[l.i] != ':' {
			return nil, false
		}
		l.i++
		l.ws()
		if bytes.Equal(k, keyAlg) {
			v, ok := l.str()
			if !ok {
				return nil, false
			}
			return v, true
		}
		if !l.skipValue() {
			return nil, false
		}
		l.ws()
		if l.i >= len(l.b) {
			return nil, false
		}
		switch l.b[l.i] {
		case ',':
			l.i++
		case '}':
			return nil, false
		default:
			return nil, false
		}
	}
}

type jwtFields struct {
	sub      []byte
	username []byte
	subStr   bool
	userStr  bool
	hasSub   bool
	hasUser  bool
	exp      float64
	hasExp   bool
	expBad   bool
	nbf      float64
	hasNbf   bool
	nbfBad   bool
	bad      bool
}

// numField parses a numeric field, treats null as absent, and flags anything
// else (string/bool/array/object) as a type error.
func (l *jlex) numField(dst *float64, has, bad, structBad *bool) {
	if l.i < len(l.b) && (l.b[l.i] == '-' || (l.b[l.i] >= '0' && l.b[l.i] <= '9')) {
		v, ok := l.num()
		if !ok {
			*structBad = true
			return
		}
		*dst, *has = v, true
		return
	}
	if l.i+4 <= len(l.b) && string(l.b[l.i:l.i+4]) == "null" {
		l.i += 4
		return
	}
	*bad = true
	if !l.skipValue() {
		*structBad = true
	}
}

// scanPayload extracts sub/username/exp/nbf from a JWT payload object.
func scanPayload(pb []byte, f *jwtFields) {
	l := jlex{b: pb}
	l.ws()
	if l.i >= len(l.b) || l.b[l.i] != '{' {
		f.bad = true
		return
	}
	l.i++
	l.ws()
	if l.i < len(l.b) && l.b[l.i] == '}' {
		return
	}
	for {
		l.ws()
		k, ok := l.str()
		if !ok {
			f.bad = true
			return
		}
		l.ws()
		if l.i >= len(l.b) || l.b[l.i] != ':' {
			f.bad = true
			return
		}
		l.i++
		l.ws()
		switch {
		case bytes.Equal(k, keySub):
			f.hasSub = true
			if l.i < len(l.b) && l.b[l.i] == '"' {
				v, ok := l.str()
				if !ok {
					f.bad = true
					return
				}
				f.sub, f.subStr = v, true
			} else if !l.skipValue() {
				f.bad = true
				return
			}
		case bytes.Equal(k, keyUsername):
			f.hasUser = true
			if l.i < len(l.b) && l.b[l.i] == '"' {
				v, ok := l.str()
				if !ok {
					f.bad = true
					return
				}
				f.username, f.userStr = v, true
			} else if !l.skipValue() {
				f.bad = true
				return
			}
		case bytes.Equal(k, keyExp):
			l.numField(&f.exp, &f.hasExp, &f.expBad, &f.bad)
		case bytes.Equal(k, keyNbf):
			l.numField(&f.nbf, &f.hasNbf, &f.nbfBad, &f.bad)
		default:
			if !l.skipValue() {
				f.bad = true
				return
			}
		}
		if f.bad {
			return
		}
		l.ws()
		if l.i >= len(l.b) {
			f.bad = true
			return
		}
		switch l.b[l.i] {
		case ',':
			l.i++
		case '}':
			return
		default:
			f.bad = true
			return
		}
	}
}

const (
	bodyOK = iota
	bodyMalformed
	bodyRequired
)

// extractBodyJSON returns the unescaped value of the "body" field, or a status
// distinguishing malformed JSON from a missing/non-string body.
func extractBodyJSON(b []byte) ([]byte, int) {
	l := jlex{b: b}
	l.ws()
	if l.i >= len(l.b) {
		return nil, bodyMalformed
	}
	if l.b[l.i] != '{' {
		// A valid non-object JSON value -> "body is required"; otherwise malformed.
		if !l.skipValue() {
			return nil, bodyMalformed
		}
		l.ws()
		if l.i != len(l.b) {
			return nil, bodyMalformed
		}
		return nil, bodyRequired
	}
	l.i++
	l.ws()
	if l.i < len(l.b) && l.b[l.i] == '}' {
		l.i++
		l.ws()
		if l.i != len(l.b) {
			return nil, bodyMalformed
		}
		return nil, bodyRequired
	}
	var body []byte
	bodyStr := false
	hasBody := false
	for {
		l.ws()
		k, ok := l.str()
		if !ok {
			return nil, bodyMalformed
		}
		l.ws()
		if l.i >= len(l.b) || l.b[l.i] != ':' {
			return nil, bodyMalformed
		}
		l.i++
		l.ws()
		if bytes.Equal(k, keyBody) {
			hasBody = true
			if l.i < len(l.b) && l.b[l.i] == '"' {
				v, ok := l.str()
				if !ok {
					return nil, bodyMalformed
				}
				body, bodyStr = v, true
			} else if !l.skipValue() {
				return nil, bodyMalformed
			}
		} else if !l.skipValue() {
			return nil, bodyMalformed
		}
		l.ws()
		if l.i >= len(l.b) {
			return nil, bodyMalformed
		}
		switch l.b[l.i] {
		case ',':
			l.i++
		case '}':
			l.i++
			l.ws()
			if l.i != len(l.b) {
				return nil, bodyMalformed
			}
			if !hasBody || !bodyStr {
				return nil, bodyRequired
			}
			return body, bodyOK
		default:
			return nil, bodyMalformed
		}
	}
}
