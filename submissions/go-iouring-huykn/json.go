package main

import (
	"unicode/utf8"
)

// ---- output -----------------------------------------------------------------------------------

// needsEsc marks the bytes JSON strings must escape.
var needsEsc = func() (t [256]bool) {
	for i := 0; i < 0x20; i++ {
		t[i] = true
	}
	t['"'] = true
	t['\\'] = true
	return
}()

const hexDigits = "0123456789abcdef"

const (
	lo7 = 0x0101010101010101
	hi7 = 0x8080808080808080
)

// plain8 reports whether none of the 8 bytes in w needs escaping (no '"', '\\' or control
// character). Bytes >= 0x80 never need it.
func plain8(w uint64) bool {
	ctl := (w - lo7*0x20) &^ w // high bit set where a byte < 0x20 (or a borrow from one)
	q := w ^ (lo7 * '"')
	quo := (q - lo7) &^ q // high bit where a byte == '"'
	bs := w ^ (lo7 * '\\')
	bsl := (bs - lo7) &^ bs // high bit where a byte == '\\'
	return (ctl|quo|bsl)&hi7 == 0
}

// appendEsc appends s as the inside of a JSON string. Text without anything to escape (the
// usual case) is checked 8 bytes at a time and copied in one piece.
func appendEsc(b, s []byte) []byte {
	i := 0
	for ; i+8 <= len(s); i += 8 {
		if !plain8(le64(s[i:])) {
			break
		}
	}
	for ; i < len(s) && !needsEsc[s[i]]; i++ {
	}
	if i == len(s) {
		return append(b, s...)
	}
	return appendEscSlow(b, s)
}

func le64(b []byte) uint64 {
	_ = b[7]
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56
}

func appendEscSlow(b, s []byte) []byte {
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !needsEsc[c] {
			continue
		}
		b = append(b, s[start:i]...)
		switch c {
		case '"':
			b = append(b, '\\', '"')
		case '\\':
			b = append(b, '\\', '\\')
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&15])
		}
		start = i + 1
	}
	return append(b, s[start:]...)
}

func appendInt(b []byte, v int64) []byte {
	if v < 0 {
		b = append(b, '-')
		v = -v
	}
	var d [20]byte
	i := len(d)
	for {
		i--
		d[i] = byte('0' + v%10)
		v /= 10
		if v == 0 {
			break
		}
	}
	return append(b, d[i:]...)
}

// ---- input ------------------------------------------------------------------------------------

// jsonReader validates a whole JSON text and reports the top-level members of an object.
type jsonReader struct {
	s   []byte
	i   int
	err bool
}

func (r *jsonReader) ws() {
	for r.i < len(r.s) {
		switch r.s[r.i] {
		case ' ', '\t', '\n', '\r':
			r.i++
		default:
			return
		}
	}
}

// str reads a string starting at its opening quote; it returns the raw bytes between the quotes
// and whether they contain escapes.
func (r *jsonReader) str() ([]byte, bool) {
	r.i++ // opening quote
	start := r.i
	esc := false
	for r.i < len(r.s) {
		c := r.s[r.i]
		switch {
		case c == '"':
			v := r.s[start:r.i]
			r.i++
			return v, esc
		case c == '\\':
			esc = true
			if r.i+1 >= len(r.s) {
				r.err = true
				return nil, false
			}
			switch r.s[r.i+1] {
			case '"', '\\', '/', 'b', 'f', 'n', 'r', 't':
				r.i += 2
			case 'u':
				if r.i+6 > len(r.s) {
					r.err = true
					return nil, false
				}
				for _, h := range r.s[r.i+2 : r.i+6] {
					if unhex(h) < 0 {
						r.err = true
						return nil, false
					}
				}
				r.i += 6
			default:
				r.err = true
				return nil, false
			}
		case c < 0x20:
			r.err = true
			return nil, false
		case c < 0x80:
			r.i++
		default:
			rn, sz := utf8.DecodeRune(r.s[r.i:])
			if rn == utf8.RuneError && sz <= 1 {
				r.err = true
				return nil, false
			}
			r.i += sz
		}
	}
	r.err = true
	return nil, false
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

func (r *jsonReader) lit(w string) {
	if len(r.s)-r.i >= len(w) && string(r.s[r.i:r.i+len(w)]) == w {
		r.i += len(w)
		return
	}
	r.err = true
}

// num reads a JSON number and returns its bytes.
func (r *jsonReader) num() []byte {
	start := r.i
	if r.i < len(r.s) && r.s[r.i] == '-' {
		r.i++
	}
	if r.i >= len(r.s) {
		r.err = true
		return nil
	}
	if r.s[r.i] == '0' {
		r.i++
	} else if r.s[r.i] >= '1' && r.s[r.i] <= '9' {
		for r.i < len(r.s) && r.s[r.i] >= '0' && r.s[r.i] <= '9' {
			r.i++
		}
	} else {
		r.err = true
		return nil
	}
	if r.i < len(r.s) && r.s[r.i] == '.' {
		r.i++
		d := r.i
		for r.i < len(r.s) && r.s[r.i] >= '0' && r.s[r.i] <= '9' {
			r.i++
		}
		if r.i == d {
			r.err = true
			return nil
		}
	}
	if r.i < len(r.s) && (r.s[r.i] == 'e' || r.s[r.i] == 'E') {
		r.i++
		if r.i < len(r.s) && (r.s[r.i] == '+' || r.s[r.i] == '-') {
			r.i++
		}
		d := r.i
		for r.i < len(r.s) && r.s[r.i] >= '0' && r.s[r.i] <= '9' {
			r.i++
		}
		if r.i == d {
			r.err = true
			return nil
		}
	}
	return r.s[start:r.i]
}

// Value kinds reported for top-level members.
const (
	jStr = iota
	jNum
	jOther
)

// value skips one value (validating it), returning its kind and, for strings and numbers, its bytes.
func (r *jsonReader) value(depth int) (int, []byte, bool) {
	r.ws()
	if r.i >= len(r.s) || depth > 64 {
		r.err = true
		return jOther, nil, false
	}
	switch c := r.s[r.i]; {
	case c == '"':
		v, esc := r.str()
		return jStr, v, esc
	case c == '{':
		r.i++
		r.ws()
		if r.i < len(r.s) && r.s[r.i] == '}' {
			r.i++
			return jOther, nil, false
		}
		for !r.err {
			r.ws()
			if r.i >= len(r.s) || r.s[r.i] != '"' {
				r.err = true
				break
			}
			r.str()
			r.ws()
			if r.i >= len(r.s) || r.s[r.i] != ':' {
				r.err = true
				break
			}
			r.i++
			r.value(depth + 1)
			r.ws()
			if r.i < len(r.s) && r.s[r.i] == ',' {
				r.i++
				continue
			}
			if r.i < len(r.s) && r.s[r.i] == '}' {
				r.i++
				break
			}
			r.err = true
		}
		return jOther, nil, false
	case c == '[':
		r.i++
		r.ws()
		if r.i < len(r.s) && r.s[r.i] == ']' {
			r.i++
			return jOther, nil, false
		}
		for !r.err {
			r.value(depth + 1)
			r.ws()
			if r.i < len(r.s) && r.s[r.i] == ',' {
				r.i++
				continue
			}
			if r.i < len(r.s) && r.s[r.i] == ']' {
				r.i++
				break
			}
			r.err = true
		}
		return jOther, nil, false
	case c == 't':
		r.lit("true")
	case c == 'f':
		r.lit("false")
	case c == 'n':
		r.lit("null")
	default:
		return jNum, r.num(), false
	}
	return jOther, nil, false
}

// member is a top-level member of an object (the last duplicate wins, as in JSON.parse).
type member struct {
	val   []byte
	kind  int
	found bool
	esc   bool
}

// object validates s as one JSON value. ok is false if s isn't valid JSON; isObj tells whether
// it is an object. Members named by keys are reported in ms (keys must not need unescaping to
// match; an escaped key is decoded before comparing).
func object(s []byte, keys []string, ms []member, scratch []byte) (ok, isObj bool) {
	r := jsonReader{s: s}
	for i := range ms {
		ms[i] = member{}
	}
	r.ws()
	if r.i < len(s) && s[r.i] == '{' {
		isObj = true
		r.i++
		r.ws()
		if r.i < len(s) && s[r.i] == '}' {
			r.i++
		} else {
			for !r.err {
				r.ws()
				if r.i >= len(s) || s[r.i] != '"' {
					r.err = true
					break
				}
				k, kesc := r.str()
				if r.err {
					break
				}
				if kesc {
					k = unescape(k, scratch[:0])
				}
				r.ws()
				if r.i >= len(s) || s[r.i] != ':' {
					r.err = true
					break
				}
				r.i++
				kind, v, esc := r.value(1)
				for j, name := range keys {
					if string(k) == name {
						ms[j] = member{val: v, kind: kind, found: true, esc: esc}
					}
				}
				r.ws()
				if r.i < len(s) && s[r.i] == ',' {
					r.i++
					continue
				}
				if r.i < len(s) && s[r.i] == '}' {
					r.i++
					break
				}
				r.err = true
			}
		}
	} else {
		r.value(0)
	}
	r.ws()
	if r.err || r.i != len(s) {
		return false, false
	}
	return true, isObj
}

// unescape decodes the inside of a validated JSON string into dst.
func unescape(s, dst []byte) []byte {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c != '\\' {
			dst = append(dst, c)
			continue
		}
		i++
		switch s[i] {
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
			r := rune(hex4(s[i+1:]))
			i += 4
			if r >= 0xd800 && r < 0xdc00 && i+6 < len(s) && s[i+1] == '\\' && s[i+2] == 'u' {
				lo := rune(hex4(s[i+3:]))
				if lo >= 0xdc00 && lo < 0xe000 {
					r = 0x10000 + (r-0xd800)<<10 + (lo - 0xdc00)
					i += 6
				}
			}
			if r >= 0xd800 && r < 0xe000 {
				r = utf8.RuneError // a lone surrogate, as JavaScript would keep; stored as U+FFFD
			}
			dst = utf8.AppendRune(dst, r)
		default: // " \ /
			dst = append(dst, s[i])
		}
	}
	return dst
}

func hex4(s []byte) int {
	return unhex(s[0])<<12 | unhex(s[1])<<8 | unhex(s[2])<<4 | unhex(s[3])
}
