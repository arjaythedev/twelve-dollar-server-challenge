package main

import "strconv"

// Minimal JSON scanning for JWT header/payload, avoiding encoding/json on the
// auth hot path. We only ever need four fields: header "alg", and payload
// "exp", "sub", "username". These scanners handle exactly the JSON that real
// tokens contain (flat objects of strings/numbers) and conservatively report
// "not found / wrong shape" for anything they don't understand, so the caller
// falls back to the same error responses as before.

// jwtAlg extracts the "alg" string from a decoded JWT header.
// Returns (alg, ok).
func jwtAlg(b []byte) (string, bool) {
	v, kind, ok := jsonField(b, "alg")
	if !ok || kind != jStr {
		return "", false
	}
	return string(v), true
}

// payloadFields extracts exp, sub, username from a decoded JWT payload in one
// pass-ish scan (three field lookups over the same bytes).
//
// Returns:
//   expOK   — "exp" present and numeric
//   exp     — its integer value (seconds)
//   subStr  — raw "sub" string value (only valid if subIsString)
//   subIsString
//   nameStr — raw "username" string value (only valid if nameIsString)
//   nameIsString
func payloadFields(b []byte) (exp int64, expOK bool, subStr string, subIsString bool, nameStr string, nameIsString bool) {
	if v, kind, ok := jsonField(b, "exp"); ok && kind == jNum {
		exp, expOK = parseJSONInt(v)
	}
	if v, kind, ok := jsonField(b, "sub"); ok {
		if kind == jStr {
			subStr, subIsString = string(v), true
		}
	}
	if v, kind, ok := jsonField(b, "username"); ok {
		if kind == jStr {
			nameStr, nameIsString = string(v), true
		}
	}
	return
}

type jKind int

const (
	jNone jKind = iota
	jStr        // value bytes are the unescaped string contents
	jNum        // value bytes are the raw numeric literal
	jOther      // object/array/bool/null — present but not a scalar we decode
)

// jsonField finds a top-level key in a flat JSON object and returns its value.
// For strings, the returned bytes are the UNESCAPED contents (common-case
// escapes handled). For numbers, the raw literal. It is deliberately strict:
// anything it can't parse cleanly yields ok=false, so callers treat it as
// absent/invalid and fall back to the existing error path.
func jsonField(b []byte, key string) ([]byte, jKind, bool) {
	i := 0
	n := len(b)
	skipWS := func() {
		for i < n {
			switch b[i] {
			case ' ', '\t', '\n', '\r':
				i++
			default:
				return
			}
		}
	}
	skipWS()
	if i >= n || b[i] != '{' {
		return nil, jNone, false
	}
	i++
	for {
		skipWS()
		if i >= n {
			return nil, jNone, false
		}
		if b[i] == '}' {
			return nil, jNone, false // key not found
		}
		if b[i] != '"' {
			return nil, jNone, false
		}
		// read key
		k, ni, ok := scanJSONString(b, i)
		if !ok {
			return nil, jNone, false
		}
		i = ni
		skipWS()
		if i >= n || b[i] != ':' {
			return nil, jNone, false
		}
		i++
		skipWS()
		if i >= n {
			return nil, jNone, false
		}
		match := string(k) == key
		// read value
		switch b[i] {
		case '"':
			v, ni, ok := scanJSONString(b, i)
			if !ok {
				return nil, jNone, false
			}
			i = ni
			if match {
				return v, jStr, true
			}
		case '{', '[':
			ni, ok := skipJSONContainer(b, i)
			if !ok {
				return nil, jNone, false
			}
			i = ni
			if match {
				return nil, jOther, true
			}
		case 't', 'f', 'n':
			ni, ok := skipJSONLiteral(b, i)
			if !ok {
				return nil, jNone, false
			}
			i = ni
			if match {
				return nil, jOther, true
			}
		default: // number
			start := i
			for i < n {
				c := b[i]
				if (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' {
					i++
				} else {
					break
				}
			}
			if i == start {
				return nil, jNone, false
			}
			if match {
				return b[start:i], jNum, true
			}
		}
		skipWS()
		if i >= n {
			return nil, jNone, false
		}
		if b[i] == ',' {
			i++
			continue
		}
		if b[i] == '}' {
			return nil, jNone, false // key not found
		}
		return nil, jNone, false
	}
}

// scanJSONString reads a JSON string starting at b[start]=='"' and returns the
// unescaped contents, the index just past the closing quote, and ok.
// Handles the standard escapes; for our inputs (ASCII usernames, integer sub,
// "HS256") this is complete. Unsupported \u escapes cause ok=false.
func scanJSONString(b []byte, start int) ([]byte, int, bool) {
	n := len(b)
	if start >= n || b[start] != '"' {
		return nil, start, false
	}
	i := start + 1
	// Fast path: no escapes.
	simpleStart := i
	for i < n {
		c := b[i]
		if c == '"' {
			return b[simpleStart:i], i + 1, true
		}
		if c == '\\' {
			break
		}
		i++
	}
	if i >= n {
		return nil, start, false
	}
	// Slow path: has escapes; build decoded buffer.
	out := make([]byte, 0, n-start)
	out = append(out, b[simpleStart:i]...)
	for i < n {
		c := b[i]
		if c == '"' {
			return out, i + 1, true
		}
		if c == '\\' {
			i++
			if i >= n {
				return nil, start, false
			}
			switch b[i] {
			case '"':
				out = append(out, '"')
			case '\\':
				out = append(out, '\\')
			case '/':
				out = append(out, '/')
			case 'n':
				out = append(out, '\n')
			case 't':
				out = append(out, '\t')
			case 'r':
				out = append(out, '\r')
			case 'b':
				out = append(out, '\b')
			case 'f':
				out = append(out, '\f')
			default:
				// \u or unknown escape: bail to the safe "invalid" path.
				return nil, start, false
			}
			i++
			continue
		}
		out = append(out, c)
		i++
	}
	return nil, start, false
}

func skipJSONContainer(b []byte, start int) (int, bool) {
	n := len(b)
	open := b[start]
	var close byte
	if open == '{' {
		close = '}'
	} else {
		close = ']'
	}
	depth := 0
	i := start
	for i < n {
		switch b[i] {
		case '"':
			_, ni, ok := scanJSONString(b, i)
			if !ok {
				return start, false
			}
			i = ni
			continue
		case open:
			depth++
		case close:
			depth--
			if depth == 0 {
				return i + 1, true
			}
		}
		i++
	}
	return start, false
}

func skipJSONLiteral(b []byte, start int) (int, bool) {
	n := len(b)
	i := start
	for i < n {
		c := b[i]
		if (c >= 'a' && c <= 'z') {
			i++
		} else {
			break
		}
	}
	if i == start {
		return start, false
	}
	return i, true
}

// parseJSONInt parses an integer from a numeric literal (truncating any
// fractional/exponent part the same way the old json.Number->Float64->int64
// path did for exp).
func parseJSONInt(lit []byte) (int64, bool) {
	// Handle plain integers fast; fall back to float semantics for . / e.
	hasDotOrExp := false
	for _, c := range lit {
		if c == '.' || c == 'e' || c == 'E' {
			hasDotOrExp = true
			break
		}
	}
	if !hasDotOrExp {
		neg := false
		i := 0
		if i < len(lit) && (lit[i] == '-' || lit[i] == '+') {
			neg = lit[i] == '-'
			i++
		}
		if i >= len(lit) {
			return 0, false
		}
		var v int64
		for ; i < len(lit); i++ {
			c := lit[i]
			if c < '0' || c > '9' {
				return 0, false
			}
			v = v*10 + int64(c-'0')
		}
		if neg {
			v = -v
		}
		return v, true
	}
	// Rare: exp given as a float/exponent literal. Match old behavior:
	// json.Number -> Float64 -> int64.
	f, err := strconv.ParseFloat(string(lit), 64)
	if err != nil {
		return 0, false
	}
	return int64(f), true
}
