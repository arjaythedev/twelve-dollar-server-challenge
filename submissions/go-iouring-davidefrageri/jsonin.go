package main

import "unicode/utf8"

// A validating JSON reader for request bodies and token payloads. object() checks the whole
// text is one JSON value (what JSON.parse accepts: no trailing commas, no comments, no leading
// zeros, no control characters in strings) and, when it is an object, picks out the top-level
// members named in keys. Values are returned as raw slices into the input; strings without
// escapes are ready to use, the others go through unescape. A later duplicate key wins, as in
// JavaScript.

type member struct {
	found bool
	kind  byte
	esc   bool
	val   []byte
}

const (
	jStr = iota + 1
	jNum
	jOther // true, false, null, array or object
)

const maxDepth = 128

type jreader struct {
	b   []byte
	i   int
	bad bool
}

func (j *jreader) ws() {
	for j.i < len(j.b) {
		switch j.b[j.i] {
		case ' ', '\t', '\n', '\r':
			j.i++
		default:
			return
		}
	}
}

// str scans a string whose opening quote is at j.i; returns the content and whether it has
// escapes.
func (j *jreader) str() ([]byte, bool) {
	j.i++
	start := j.i
	esc := false
	for j.i < len(j.b) {
		c := j.b[j.i]
		switch {
		case c == '"':
			s := j.b[start:j.i]
			j.i++
			return s, esc
		case c == '\\':
			esc = true
			j.i++
			if j.i >= len(j.b) {
				j.bad = true
				return nil, false
			}
			switch j.b[j.i] {
			case '"', '\\', '/', 'b', 'f', 'n', 'r', 't':
				j.i++
			case 'u':
				if j.i+4 >= len(j.b) {
					j.bad = true
					return nil, false
				}
				for k := 1; k <= 4; k++ {
					if hexVal(j.b[j.i+k]) < 0 {
						j.bad = true
						return nil, false
					}
				}
				j.i += 5
			default:
				j.bad = true
				return nil, false
			}
		case c < 0x20:
			j.bad = true
			return nil, false
		default:
			j.i++
		}
	}
	j.bad = true
	return nil, false
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

func (j *jreader) digits() bool {
	n := 0
	for j.i < len(j.b) && j.b[j.i] >= '0' && j.b[j.i] <= '9' {
		j.i++
		n++
	}
	return n > 0
}

// num scans a number at j.i and returns its text.
func (j *jreader) num() []byte {
	start := j.i
	if j.i < len(j.b) && j.b[j.i] == '-' {
		j.i++
	}
	if j.i >= len(j.b) {
		j.bad = true
		return nil
	}
	if j.b[j.i] == '0' {
		j.i++
	} else if !j.digits() {
		j.bad = true
		return nil
	}
	if j.i < len(j.b) && j.b[j.i] == '.' {
		j.i++
		if !j.digits() {
			j.bad = true
			return nil
		}
	}
	if j.i < len(j.b) && (j.b[j.i] == 'e' || j.b[j.i] == 'E') {
		j.i++
		if j.i < len(j.b) && (j.b[j.i] == '+' || j.b[j.i] == '-') {
			j.i++
		}
		if !j.digits() {
			j.bad = true
			return nil
		}
	}
	return j.b[start:j.i]
}

func (j *jreader) lit(s string) bool {
	if len(j.b)-j.i >= len(s) && string(j.b[j.i:j.i+len(s)]) == s {
		j.i += len(s)
		return true
	}
	j.bad = true
	return false
}

// value scans any value at j.i (after whitespace), validating it. kind/esc/raw describe it.
func (j *jreader) value(depth int) (kind byte, esc bool, raw []byte) {
	if j.i >= len(j.b) {
		j.bad = true
		return 0, false, nil
	}
	start := j.i
	switch c := j.b[j.i]; {
	case c == '"':
		s, e := j.str()
		return jStr, e, s
	case c == '-' || (c >= '0' && c <= '9'):
		return jNum, false, j.num()
	case c == 't':
		j.lit("true")
	case c == 'f':
		j.lit("false")
	case c == 'n':
		j.lit("null")
	case c == '{':
		if depth >= maxDepth {
			j.bad = true
			return 0, false, nil
		}
		j.i++
		j.ws()
		if j.i < len(j.b) && j.b[j.i] == '}' {
			j.i++
			break
		}
		for !j.bad {
			j.ws()
			if j.i >= len(j.b) || j.b[j.i] != '"' {
				j.bad = true
				break
			}
			j.str()
			j.ws()
			if j.i >= len(j.b) || j.b[j.i] != ':' {
				j.bad = true
				break
			}
			j.i++
			j.ws()
			j.value(depth + 1)
			j.ws()
			if j.i >= len(j.b) {
				j.bad = true
				break
			}
			if j.b[j.i] == ',' {
				j.i++
				continue
			}
			if j.b[j.i] == '}' {
				j.i++
				break
			}
			j.bad = true
		}
	case c == '[':
		if depth >= maxDepth {
			j.bad = true
			return 0, false, nil
		}
		j.i++
		j.ws()
		if j.i < len(j.b) && j.b[j.i] == ']' {
			j.i++
			break
		}
		for !j.bad {
			j.ws()
			j.value(depth + 1)
			j.ws()
			if j.i >= len(j.b) {
				j.bad = true
				break
			}
			if j.b[j.i] == ',' {
				j.i++
				continue
			}
			if j.b[j.i] == ']' {
				j.i++
				break
			}
			j.bad = true
		}
	default:
		j.bad = true
	}
	return jOther, false, j.b[start:j.i]
}

// object validates data as one JSON value. If it is an object, ms[k] describes the last member
// named keys[k]. scratch is used to unescape keys that contain escapes.
func object(data []byte, keys []string, ms []member, scratch []byte) (valid, isObj bool) {
	for k := range ms {
		ms[k] = member{}
	}
	j := jreader{b: data}
	j.ws()
	if j.i >= len(j.b) {
		return false, false
	}
	if j.b[j.i] != '{' {
		j.value(0)
		j.ws()
		return !j.bad && j.i == len(j.b), false
	}
	j.i++
	j.ws()
	if j.i < len(j.b) && j.b[j.i] == '}' {
		j.i++
		j.ws()
		return j.i == len(j.b), true
	}
	for !j.bad {
		j.ws()
		if j.i >= len(j.b) || j.b[j.i] != '"' {
			return false, true
		}
		key, kesc := j.str()
		if j.bad {
			return false, true
		}
		if kesc {
			scratch = unescape(key, scratch[:0])
			key = scratch
		}
		j.ws()
		if j.i >= len(j.b) || j.b[j.i] != ':' {
			return false, true
		}
		j.i++
		j.ws()
		kind, esc, raw := j.value(1)
		if j.bad {
			return false, true
		}
		for k, name := range keys {
			if string(key) == name {
				ms[k] = member{found: true, kind: kind, esc: esc, val: raw}
			}
		}
		j.ws()
		if j.i >= len(j.b) {
			return false, true
		}
		if j.b[j.i] == ',' {
			j.i++
			continue
		}
		if j.b[j.i] == '}' {
			j.i++
			j.ws()
			return j.i == len(j.b), true
		}
		return false, true
	}
	return false, true
}

// unescape decodes a validated JSON string's content onto dst. Lone surrogates become U+FFFD.
func unescape(s, dst []byte) []byte {
	for i := 0; i < len(s); {
		c := s[i]
		if c != '\\' {
			dst = append(dst, c)
			i++
			continue
		}
		i++
		switch s[i] {
		case '"':
			dst = append(dst, '"')
		case '\\':
			dst = append(dst, '\\')
		case '/':
			dst = append(dst, '/')
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
			r := rune(hexVal(s[i+1])<<12 | hexVal(s[i+2])<<8 | hexVal(s[i+3])<<4 | hexVal(s[i+4]))
			i += 4
			if r >= 0xd800 && r < 0xdc00 {
				if i+6 < len(s) && s[i+1] == '\\' && s[i+2] == 'u' {
					lo := rune(hexVal(s[i+3])<<12 | hexVal(s[i+4])<<8 | hexVal(s[i+5])<<4 | hexVal(s[i+6]))
					if lo >= 0xdc00 && lo < 0xe000 {
						r = 0x10000 + (r-0xd800)<<10 + (lo - 0xdc00)
						i += 6
					} else {
						r = utf8.RuneError
					}
				} else {
					r = utf8.RuneError
				}
			} else if r >= 0xdc00 && r < 0xe000 {
				r = utf8.RuneError
			}
			dst = utf8.AppendRune(dst, r)
		}
		i++
	}
	return dst
}
