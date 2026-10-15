package main

import "unicode/utf8"

// A minimal allocation-free JSON reader: enough to walk the top-level members of an object and
// unescape string values. Used for JWT header/payload and the create-post body.

const (
	kindString byte = 's'
	kindNumber byte = 'n'
	kindOther  byte = 'o' // true/false/null/array/object
)

func skipWS(b []byte, i int) int {
	for i < len(b) && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') {
		i++
	}
	return i
}

// scanString returns the index just past the closing quote of the string starting at b[i] == '"'.
func scanString(b []byte, i int) (int, bool) {
	i++
	for i < len(b) {
		c := b[i]
		switch {
		case c == '"':
			return i + 1, true
		case c == '\\':
			i += 2
		case c < 0x20:
			return 0, false
		default:
			i++
		}
	}
	return 0, false
}

func scanValue(b []byte, i, depth int) (int, byte, bool) {
	if i >= len(b) || depth > 32 {
		return 0, 0, false
	}
	switch c := b[i]; {
	case c == '"':
		n, ok := scanString(b, i)
		return n, kindString, ok
	case c == '{':
		n, ok := scanObject(b, i, depth+1, nil)
		return n, kindOther, ok
	case c == '[':
		i = skipWS(b, i+1)
		if i < len(b) && b[i] == ']' {
			return i + 1, kindOther, true
		}
		for {
			var ok bool
			if i, _, ok = scanValue(b, i, depth+1); !ok {
				return 0, 0, false
			}
			i = skipWS(b, i)
			if i >= len(b) {
				return 0, 0, false
			}
			if b[i] == ']' {
				return i + 1, kindOther, true
			}
			if b[i] != ',' {
				return 0, 0, false
			}
			i = skipWS(b, i+1)
		}
	case c == '-' || (c >= '0' && c <= '9'):
		j := i + 1
		for j < len(b) && (b[j] >= '0' && b[j] <= '9' || b[j] == '.' || b[j] == 'e' || b[j] == 'E' || b[j] == '+' || b[j] == '-') {
			j++
		}
		return j, kindNumber, true
	case c == 't' || c == 'f' || c == 'n':
		for _, lit := range []string{"true", "false", "null"} {
			if len(b)-i >= len(lit) && string(b[i:i+len(lit)]) == lit {
				return i + len(lit), kindOther, true
			}
		}
	}
	return 0, 0, false
}

// scanObject walks the object starting at b[i] == '{' and returns the index past '}'. For every
// member it calls fn (when non-nil) with the raw key (between quotes, not unescaped), the raw value
// (for strings: between quotes) and the value kind.
func scanObject(b []byte, i, depth int, fn func(key, val []byte, kind byte)) (int, bool) {
	if i >= len(b) || b[i] != '{' {
		return 0, false
	}
	i = skipWS(b, i+1)
	if i < len(b) && b[i] == '}' {
		return i + 1, true
	}
	for {
		if i >= len(b) || b[i] != '"' {
			return 0, false
		}
		kend, ok := scanString(b, i)
		if !ok {
			return 0, false
		}
		key := b[i+1 : kend-1]
		i = skipWS(b, kend)
		if i >= len(b) || b[i] != ':' {
			return 0, false
		}
		i = skipWS(b, i+1)
		vend, kind, ok := scanValue(b, i, depth)
		if !ok {
			return 0, false
		}
		if fn != nil {
			val := b[i:vend]
			if kind == kindString {
				val = b[i+1 : vend-1]
			}
			fn(key, val, kind)
		}
		i = skipWS(b, vend)
		if i >= len(b) {
			return 0, false
		}
		if b[i] == '}' {
			return i + 1, true
		}
		if b[i] != ',' {
			return 0, false
		}
		i = skipWS(b, i+1)
	}
}

// parseObject validates that b is exactly one JSON object (plus surrounding whitespace) and
// reports its top-level members to fn.
func parseObject(b []byte, fn func(key, val []byte, kind byte)) bool {
	i := skipWS(b, 0)
	end, ok := scanObject(b, i, 0, fn)
	if !ok {
		return false
	}
	return skipWS(b, end) == len(b)
}

func hexVal(c byte) (uint16, bool) {
	switch {
	case c >= '0' && c <= '9':
		return uint16(c - '0'), true
	case c >= 'a' && c <= 'f':
		return uint16(c-'a') + 10, true
	case c >= 'A' && c <= 'F':
		return uint16(c-'A') + 10, true
	}
	return 0, false
}

func hex4(b []byte) (uint16, bool) {
	if len(b) < 4 {
		return 0, false
	}
	var v uint16
	for k := range 4 {
		h, ok := hexVal(b[k])
		if !ok {
			return 0, false
		}
		v = v<<4 | h
	}
	return v, true
}

// unescape appends the decoded form of raw (string contents between quotes) to dst.
func unescape(dst, raw []byte) ([]byte, bool) {
	for i := 0; i < len(raw); {
		c := raw[i]
		if c != '\\' {
			dst = append(dst, c)
			i++
			continue
		}
		i++
		if i >= len(raw) {
			return dst, false
		}
		switch raw[i] {
		case '"', '\\', '/':
			dst = append(dst, raw[i])
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
			r, ok := hex4(raw[i+1:])
			if !ok {
				return dst, false
			}
			i += 4
			cp := rune(r)
			if r >= 0xD800 && r < 0xDC00 { // high surrogate: needs \uDC00-\uDFFF next
				if i+6 < len(raw)+0 && raw[i+1] == '\\' && raw[i+2] == 'u' {
					lo, ok := hex4(raw[i+3:])
					if ok && lo >= 0xDC00 && lo < 0xE000 {
						cp = 0x10000 + (rune(r)-0xD800)<<10 + (rune(lo) - 0xDC00)
						i += 6
					} else {
						cp = utf8.RuneError
					}
				} else {
					cp = utf8.RuneError
				}
			} else if r >= 0xDC00 && r < 0xE000 {
				cp = utf8.RuneError
			}
			dst = utf8.AppendRune(dst, cp)
		default:
			return dst, false
		}
		i++
	}
	return dst, true
}
