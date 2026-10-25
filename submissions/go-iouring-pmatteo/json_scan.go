package main

import "unicode/utf8"

// A scanner for the flat JSON objects this server has to read: JWT headers, JWT payloads and
// {"body":"..."} request bodies. It allocates nothing and returns views into the input.

type jsonKind uint8

const (
	jsonAbsent jsonKind = iota
	jsonString
	jsonNumber
	jsonOther // true, false, null, object, array
)

// jsonField finds a top-level key and returns its value. For jsonString the value is unquoted
// but still escaped; use jsonUnescape when the bytes are going somewhere that cares.
func jsonField(b []byte, key string) ([]byte, jsonKind) {
	i := 0
	n := len(b)
	for i < n && isSpace(b[i]) {
		i++
	}
	if i >= n || b[i] != '{' {
		return nil, jsonAbsent
	}
	i++
	for {
		for i < n && isSpace(b[i]) {
			i++
		}
		if i >= n || b[i] == '}' {
			return nil, jsonAbsent
		}
		if b[i] != '"' {
			return nil, jsonAbsent
		}
		kStart := i + 1
		kEnd, ok := scanStringEnd(b, kStart)
		if !ok {
			return nil, jsonAbsent
		}
		i = kEnd + 1
		for i < n && isSpace(b[i]) {
			i++
		}
		if i >= n || b[i] != ':' {
			return nil, jsonAbsent
		}
		i++
		for i < n && isSpace(b[i]) {
			i++
		}
		if i >= n {
			return nil, jsonAbsent
		}
		vStart := i
		var kind jsonKind
		switch {
		case b[i] == '"':
			vStart = i + 1
			end, ok := scanStringEnd(b, vStart)
			if !ok {
				return nil, jsonAbsent
			}
			i = end + 1
			kind = jsonString
			if string(b[kStart:kEnd]) == key {
				return b[vStart:end], kind
			}
		case b[i] == '-' || (b[i] >= '0' && b[i] <= '9'):
			for i < n && b[i] != ',' && b[i] != '}' && !isSpace(b[i]) {
				i++
			}
			kind = jsonNumber
			if string(b[kStart:kEnd]) == key {
				return b[vStart:i], kind
			}
		case b[i] == '{' || b[i] == '[':
			end, ok := scanNestedEnd(b, i)
			if !ok {
				return nil, jsonAbsent
			}
			i = end
			kind = jsonOther
			if string(b[kStart:kEnd]) == key {
				return b[vStart:i], kind
			}
		default:
			for i < n && b[i] != ',' && b[i] != '}' && !isSpace(b[i]) {
				i++
			}
			kind = jsonOther
			if string(b[kStart:kEnd]) == key {
				return b[vStart:i], kind
			}
		}
		for i < n && isSpace(b[i]) {
			i++
		}
		if i >= n {
			return nil, jsonAbsent
		}
		if b[i] == ',' {
			i++
			continue
		}
		return nil, jsonAbsent
	}
}

// jsonValid reports whether b is a syntactically valid JSON object, which is the only shape the
// POST /posts route accepts. It is the check behind "malformed JSON body".
func jsonValid(b []byte) bool {
	i := skipSpace(b, 0)
	end, ok := scanValue(b, i)
	if !ok {
		return false
	}
	return skipSpace(b, end) == len(b) && b[i] == '{'
}

func skipSpace(b []byte, i int) int {
	for i < len(b) && isSpace(b[i]) {
		i++
	}
	return i
}

func isSpace(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' }

// scanStringEnd takes the index just after the opening quote and returns the index of the
// closing quote.
func scanStringEnd(b []byte, i int) (int, bool) {
	for i < len(b) {
		switch b[i] {
		case '\\':
			i += 2
		case '"':
			return i, true
		default:
			i++
		}
	}
	return 0, false
}

func scanNestedEnd(b []byte, i int) (int, bool) { return scanValue(b, i) }

// scanValue returns the index just past the value starting at i.
func scanValue(b []byte, i int) (int, bool) {
	n := len(b)
	if i >= n {
		return 0, false
	}
	switch c := b[i]; {
	case c == '"':
		end, ok := scanStringEnd(b, i+1)
		if !ok {
			return 0, false
		}
		return end + 1, true
	case c == '{' || c == '[':
		closer := byte('}')
		if c == '[' {
			closer = ']'
		}
		i = skipSpace(b, i+1)
		if i < n && b[i] == closer {
			return i + 1, true
		}
		for {
			if closer == '}' {
				if i >= n || b[i] != '"' {
					return 0, false
				}
				end, ok := scanStringEnd(b, i+1)
				if !ok {
					return 0, false
				}
				i = skipSpace(b, end+1)
				if i >= n || b[i] != ':' {
					return 0, false
				}
				i = skipSpace(b, i+1)
			}
			var ok bool
			i, ok = scanValue(b, i)
			if !ok {
				return 0, false
			}
			i = skipSpace(b, i)
			if i >= n {
				return 0, false
			}
			if b[i] == ',' {
				i = skipSpace(b, i+1)
				continue
			}
			if b[i] == closer {
				return i + 1, true
			}
			return 0, false
		}
	case c == 't':
		return litEnd(b, i, "true")
	case c == 'f':
		return litEnd(b, i, "false")
	case c == 'n':
		return litEnd(b, i, "null")
	case c == '-' || (c >= '0' && c <= '9'):
		j := i
		if b[j] == '-' {
			j++
		}
		start := j
		for j < n && b[j] >= '0' && b[j] <= '9' {
			j++
		}
		if j == start {
			return 0, false
		}
		if j < n && b[j] == '.' {
			j++
			start = j
			for j < n && b[j] >= '0' && b[j] <= '9' {
				j++
			}
			if j == start {
				return 0, false
			}
		}
		if j < n && (b[j] == 'e' || b[j] == 'E') {
			j++
			if j < n && (b[j] == '+' || b[j] == '-') {
				j++
			}
			start = j
			for j < n && b[j] >= '0' && b[j] <= '9' {
				j++
			}
			if j == start {
				return 0, false
			}
		}
		return j, true
	}
	return 0, false
}

func litEnd(b []byte, i int, lit string) (int, bool) {
	if i+len(lit) > len(b) || string(b[i:i+len(lit)]) != lit {
		return 0, false
	}
	return i + len(lit), true
}

// jsonUnescape decodes JSON string escapes into dst. \uXXXX surrogate pairs are combined.
func jsonUnescape(dst, src []byte) ([]byte, bool) {
	for i := 0; i < len(src); {
		c := src[i]
		if c != '\\' {
			dst = append(dst, c)
			i++
			continue
		}
		i++
		if i >= len(src) {
			return dst, false
		}
		switch src[i] {
		case '"', '\\', '/':
			dst = append(dst, src[i])
			i++
		case 'b':
			dst = append(dst, '\b')
			i++
		case 'f':
			dst = append(dst, '\f')
			i++
		case 'n':
			dst = append(dst, '\n')
			i++
		case 'r':
			dst = append(dst, '\r')
			i++
		case 't':
			dst = append(dst, '\t')
			i++
		case 'u':
			r, w, ok := decodeUnicodeEscape(src, i-1)
			if !ok {
				return dst, false
			}
			dst = utf8.AppendRune(dst, r)
			i = i - 1 + w
		default:
			return dst, false
		}
	}
	return dst, true
}

// decodeUnicodeEscape reads the \uXXXX (plus an optional low surrogate) starting at i, which
// points at the backslash. It returns the rune and how many bytes were consumed.
func decodeUnicodeEscape(src []byte, i int) (rune, int, bool) {
	hi, ok := hex4(src, i+2)
	if !ok {
		return 0, 0, false
	}
	if hi >= 0xD800 && hi <= 0xDBFF {
		if i+12 <= len(src) && src[i+6] == '\\' && src[i+7] == 'u' {
			lo, ok := hex4(src, i+8)
			if ok && lo >= 0xDC00 && lo <= 0xDFFF {
				return rune(0x10000 + (hi-0xD800)<<10 + (lo - 0xDC00)), 12, true
			}
		}
		return 0xFFFD, 6, true
	}
	if hi >= 0xDC00 && hi <= 0xDFFF {
		return 0xFFFD, 6, true
	}
	return rune(hi), 6, true
}

func hex4(b []byte, i int) (rune, bool) {
	if i+4 > len(b) {
		return 0, false
	}
	var v rune
	for k := 0; k < 4; k++ {
		c := b[i+k]
		switch {
		case c >= '0' && c <= '9':
			v = v<<4 | rune(c-'0')
		case c >= 'a' && c <= 'f':
			v = v<<4 | rune(c-'a'+10)
		case c >= 'A' && c <= 'F':
			v = v<<4 | rune(c-'A'+10)
		default:
			return 0, false
		}
	}
	return v, true
}
