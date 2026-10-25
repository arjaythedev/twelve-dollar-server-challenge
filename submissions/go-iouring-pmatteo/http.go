package main

import (
	"bytes"
	"unicode/utf8"
)

const (
	// hdrReserve is kept free at the front of a response buffer. The body is written after it
	// and the header is then written backwards into the gap, so the send needs no copy of the
	// body. 128 is checked against the longest header this file can emit in http_test.go.
	hdrReserve = 128

	maxHeadBytes = 4096 // request line plus headers
	maxBodyBytes = 8192 // Content-Length ceiling; a post is capped at 500 characters
	maxReqBytes  = maxHeadBytes + maxBodyBytes
)

const (
	errHeadTooLarge = `{"error":"request header too large"}`
	errBodyTooLarge = `{"error":"request body too large"}`
	errBadRequest   = `{"error":"malformed request"}`
	errUnsupported  = `{"error":"unsupported transfer encoding"}`
)

const continue100 = "HTTP/1.1 100 Continue\r\n\r\n"

type parseState uint8

const (
	parseIncomplete parseState = iota
	parseDone
	parseFailed
)

// parsed is one request lifted out of a connection's byte stream.
type parsed struct {
	req       request
	n         int    // bytes of the stream this request consumed
	keepAlive bool   // false means the response must be the connection's last
	expect100 bool   // the client is waiting for a 100 Continue before sending the body
	status    int    // non-zero: answer with this instead of routing
	errBody   string // the body that goes with status
}

// parseRequest reads one request from the front of b into p.
//
// p is filled in place because parsed is 136 bytes: returning it by value cost that copy twice
// per request, once out of here and once into whatever the caller passed it on to. The caller
// owns p and reuses it, so parseRequest starts by clearing it - one memclr instead of two
// copies, and without it a field left unset by this request would still hold the last one's.
//
// parseIncomplete means more bytes are needed and nothing in p is final except expect100.
// parseFailed means the stream cannot be resynchronised: answer status and close.
func parseRequest(b []byte, p *parsed) parseState {
	*p = parsed{}

	end := bytes.Index(b, []byte("\r\n\r\n"))
	if end < 0 {
		if len(b) > maxHeadBytes {
			return p.fail(431, errHeadTooLarge)
		}
		return parseIncomplete
	}
	head := b[:end]
	rest := b[end+4:]

	line := head
	if i := bytes.Index(head, []byte("\r\n")); i >= 0 {
		line, head = head[:i], head[i+2:]
	} else {
		head = nil
	}

	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return p.fail(400, errBadRequest)
	}
	sp2 := bytes.IndexByte(line[sp1+1:], ' ')
	if sp2 < 0 {
		return p.fail(400, errBadRequest)
	}
	sp2 += sp1 + 1
	target := line[sp1+1 : sp2]
	if len(target) == 0 {
		return p.fail(400, errBadRequest)
	}
	p.req.method = line[:sp1]
	// A query string is not part of any route. Cutting it here keeps the routing table exact
	// instead of having every route tolerate a suffix.
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	p.req.path = target

	switch version := line[sp2+1:]; {
	case eq(version, "HTTP/1.1"):
		p.keepAlive = true
	case eq(version, "HTTP/1.0"):
		p.keepAlive = false
	default:
		return p.fail(505, errBadRequest)
	}

	contentLength := -1
	for len(head) > 0 {
		var h []byte
		if i := bytes.Index(head, []byte("\r\n")); i >= 0 {
			h, head = head[:i], head[i+2:]
		} else {
			h, head = head, nil
		}
		c := bytes.IndexByte(h, ':')
		if c <= 0 {
			return p.fail(400, errBadRequest)
		}
		name, value := h[:c], trimSpace(h[c+1:])
		switch {
		case eqFold(name, "authorization"):
			p.req.auth = value
		case eqFold(name, "content-length"):
			v, ok := parseContentLength(value)
			// A second, different Content-Length is a request smuggling attempt, not a typo.
			if !ok || (contentLength >= 0 && contentLength != v) {
				return p.fail(400, errBadRequest)
			}
			contentLength = v
		case eqFold(name, "transfer-encoding"):
			// The one framing this server refuses. Nothing in the harness sends it, and
			// accepting it silently would be worse than saying so.
			return p.fail(501, errUnsupported)
		case eqFold(name, "connection"):
			if tokenPresent(value, "close") {
				p.keepAlive = false
			} else if tokenPresent(value, "keep-alive") {
				p.keepAlive = true
			}
		case eqFold(name, "expect"):
			p.expect100 = tokenPresent(value, "100-continue")
		}
	}

	if contentLength < 0 {
		contentLength = 0
	}
	if contentLength > maxBodyBytes {
		return p.fail(413, errBodyTooLarge)
	}
	if len(rest) < contentLength {
		return parseIncomplete
	}
	p.req.body = rest[:contentLength]
	p.n = end + 4 + contentLength
	return parseDone
}

// fail turns p into the rejection, discarding whatever was parsed before it. keepAlive stays
// false: a request this server could not parse leaves the stream at an unknown offset, so the
// only safe answer is one response and a close.
func (p *parsed) fail(status int, body string) parseState {
	*p = parsed{status: status, errBody: body}
	return parseFailed
}

// frameResponse writes the header backwards into the reserve in front of the body and returns the
// offset the response starts at. The body is never moved.
//
// buf is a response buffer whose body occupies buf[hdrReserve : hdrReserve+bodyLen], so the
// response to send is buf[frameResponse(...) : hdrReserve+bodyLen].
func frameResponse(buf []byte, bodyLen, status int, keepAlive bool) int {
	var h [hdrReserve]byte
	p := append(h[:0], statusLine(status)...)
	p = append(p, "Content-Type: application/json\r\nContent-Length: "...)
	p = appendInt(p, int64(bodyLen))
	if !keepAlive {
		p = append(p, "\r\nConnection: close"...)
	}
	p = append(p, "\r\n\r\n"...)
	start := hdrReserve - len(p)
	copy(buf[start:hdrReserve], p)
	return start
}

func statusLine(status int) string {
	switch status {
	case 200:
		return "HTTP/1.1 200 OK\r\n"
	case 201:
		return "HTTP/1.1 201 Created\r\n"
	case 400:
		return "HTTP/1.1 400 Bad Request\r\n"
	case 401:
		return "HTTP/1.1 401 Unauthorized\r\n"
	case 404:
		return "HTTP/1.1 404 Not Found\r\n"
	case 413:
		return "HTTP/1.1 413 Content Too Large\r\n"
	case 431:
		return "HTTP/1.1 431 Request Header Fields Too Large\r\n"
	case 500:
		return "HTTP/1.1 500 Internal Server Error\r\n"
	case 501:
		return "HTTP/1.1 501 Not Implemented\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	case 505:
		return "HTTP/1.1 505 HTTP Version Not Supported\r\n"
	}
	return "HTTP/1.1 500 Internal Server Error\r\n"
}

// eqFold compares a header name against a lowercase literal, ASCII case-insensitively. Header
// names are ASCII by definition, so there is no Unicode case folding to do.
func eqFold(b []byte, lower string) bool {
	if len(b) != len(lower) {
		return false
	}
	for i := range b {
		c := b[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		if c != lower[i] {
			return false
		}
	}
	return true
}

// tokenPresent reports whether a comma-separated header value contains tok.
func tokenPresent(value []byte, tok string) bool {
	for len(value) > 0 {
		t := value
		if i := bytes.IndexByte(value, ','); i >= 0 {
			t, value = value[:i], value[i+1:]
		} else {
			value = nil
		}
		if eqFold(trimSpace(t), tok) {
			return true
		}
	}
	return false
}

// parseContentLength accepts only digits: no sign, no whitespace inside, no leading plus. A
// leading zero is allowed because "0" is the common value.
func parseContentLength(b []byte) (int, bool) {
	if len(b) == 0 || len(b) > 9 {
		return 0, false
	}
	v := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int(c-'0')
	}
	return v, true
}

// ---- from bytes.go -------------------------------------------------------------------------------------

func eq(b []byte, s string) bool { return string(b) == s }

func hasPrefix(b []byte, s string) bool {
	return len(b) >= len(s) && string(b[:len(s)]) == s
}

func hasSuffix(b []byte, s string) bool {
	return len(b) >= len(s) && string(b[len(b)-len(s):]) == s
}

func isASCIISpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

func trimSpace(b []byte) []byte {
	i, j := 0, len(b)
	for i < j && isASCIISpace(b[i]) {
		i++
	}
	for j > i && isASCIISpace(b[j-1]) {
		j--
	}
	return b[i:j]
}

// runeCount counts UTF-8 sequences by their lead bytes, which is what SQLite's length() counts.
func runeCount(b []byte) int {
	n := 0
	for _, c := range b {
		if c&0xC0 != 0x80 {
			n++
		}
	}
	return n
}

func appendInt(out []byte, v int64) []byte {
	if v == 0 {
		return append(out, '0')
	}
	if v < 0 {
		out = append(out, '-')
		v = -v
	}
	var buf [20]byte
	i := len(buf)
	for v > 0 {
		i--
		buf[i] = byte('0' + v%10)
		v /= 10
	}
	return append(out, buf[i:]...)
}

const hexDigits = "0123456789abcdef"

// appendJSONString writes a quoted JSON string. Escaping <>&/ and non-ASCII is optional per the
// spec, so only the characters JSON actually requires are escaped and UTF-8 passes through.
func appendJSONString(out, s []byte) []byte {
	out = append(out, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		out = append(out, s[start:i]...)
		switch c {
		case '"':
			out = append(out, '\\', '"')
		case '\\':
			out = append(out, '\\', '\\')
		case '\n':
			out = append(out, '\\', 'n')
		case '\r':
			out = append(out, '\\', 'r')
		case '\t':
			out = append(out, '\\', 't')
		case '\b':
			out = append(out, '\\', 'b')
		case '\f':
			out = append(out, '\\', 'f')
		default:
			out = append(out, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xF])
		}
		start = i + 1
	}
	out = append(out, s[start:]...)
	return append(out, '"')
}

// ---- from json_scan.go ---------------------------------------------------------------------------------

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
