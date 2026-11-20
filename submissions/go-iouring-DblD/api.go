//go:build linux

// The five endpoints: routing, validation, and turning rows into JSON.
//
// Reads (92 % of the traffic) allocate nothing: the request is parsed in place, the rows
// arrive in a reused buffer and the JSON is appended straight into the response buffer.
// The write path (likes and new posts, 8 %) still uses the standard library's JSON decoder
// and HMAC for now; it is correct first and gets tuned in stage 6 if it shows up in a profile.
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"hash"
	"log"
	"math/bits"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// A request is handled in two steps, because the loop may have to put it aside between them
// (see the answer budget in main.go). parse does everything that needs the request's bytes:
// routing, the token, the body. What is left is a small fixed-size record with no pointers,
// so thousands of them can wait in a preallocated ring. execute does the database work and
// writes the response.
const (
	reqHealth = iota + 1
	reqFeed
	reqPost   // a = post id
	reqLike   // a = post id, b = user id
	reqCreate // a = index into server.creates (the text and the author), b = user id
	reqError  // a = index into errorReplies
)

type request struct {
	arrival int64 // UnixNano of the pass it arrived in
	a, b    int64
	slot    uint32
	gen     uint16
	ri      uint8
	kind    uint8
}

func (r *request) isWrite() bool { return r.kind == reqLike || r.kind == reqCreate }

type errorReply struct {
	status int
	msg    string
}

var errorReplies = []errorReply{
	{404, "not found"}, {400, "invalid post id"}, {401, "missing bearer token"}, {401, "invalid or expired token"},
	{401, "invalid token payload"}, {400, "malformed JSON body"}, {400, "body is required"}, {400, "body must be at most 500 characters"},
}

func refuse(status int, msg string) request {
	for i, e := range errorReplies {
		if e.status == status && e.msg == msg {
			return request{kind: reqError, a: int64(i)}
		}
	}
	panic("no such error reply: " + msg)
}

// A new post's text and author wait here until its request is executed. About 1 % of
// requests, so this path is kept simple rather than fast.
type createReq struct {
	text     []byte
	username string
}

// parse turns one request into a record. head is the request line plus headers.
func (s *server) parse(head, body []byte) request {
	line := head
	if i := bytes.IndexByte(head, '\r'); i >= 0 {
		line = head[:i]
	}
	method, rest, _ := bytes.Cut(line, []byte(" "))
	path, _, _ := bytes.Cut(rest, []byte(" "))
	path, _, _ = bytes.Cut(path, []byte("?"))
	post := string(method) == "POST" // compiles to a comparison, no allocation

	switch {
	case string(path) == "/health":
		return request{kind: reqHealth}
	case string(path) == "/feed":
		return request{kind: reqFeed}
	case string(path) == "/posts" && post:
		return s.parseCreate(head, body)
	case bytes.HasPrefix(path, []byte("/posts/")):
		rawID := path[len("/posts/"):]
		if likeID, ok := bytes.CutSuffix(rawID, []byte("/like")); ok && post {
			uid, _, errMsg := s.auth(head) // auth before the id, per the spec
			if errMsg != "" {
				return refuse(401, errMsg)
			}
			id, ok := parseID(likeID)
			if !ok {
				return refuse(400, "invalid post id")
			}
			return request{kind: reqLike, a: id, b: uid}
		}
		if bytes.IndexByte(rawID, '/') < 0 {
			id, ok := parseID(rawID)
			if !ok {
				return refuse(400, "invalid post id")
			}
			return request{kind: reqPost, a: id}
		}
	}
	return refuse(404, "not found")
}

func (s *server) parseCreate(head, body []byte) request {
	uid, username, errMsg := s.auth(head)
	if errMsg != "" {
		return refuse(401, errMsg)
	}
	var in map[string]any
	if json.Unmarshal(body, &in) != nil {
		return refuse(400, "malformed JSON body")
	}
	str, _ := in["body"].(string)
	text := strings.TrimSpace(str)
	switch {
	case text == "":
		return refuse(400, "body is required")
	case utf8.RuneCountInString(text) > 500: // characters, as SQLite's length() counts them
		return refuse(400, "body must be at most 500 characters")
	}
	c := createReq{text: []byte(text), username: string(username)}
	i := len(s.creates)
	if n := len(s.createFree); n > 0 {
		i, s.createFree = s.createFree[n-1], s.createFree[:n-1]
		s.creates[i] = c
	} else {
		s.creates = append(s.creates, c)
	}
	return request{kind: reqCreate, a: int64(i), b: uid}
}

// discard releases what a request holds if it will never be executed.
func (s *server) discard(r *request) {
	if r.kind == reqCreate {
		s.creates[r.a] = createReq{}
		s.createFree = append(s.createFree, int(r.a))
	}
}

// execute does the database work for a request and appends the full HTTP response to out.
func (s *server) execute(out []byte, r *request) []byte {
	switch r.kind {
	case reqHealth:
		return s.health(out)
	case reqFeed:
		return s.feed(out)
	case reqPost:
		return s.getPost(out, r.a)
	case reqLike:
		return s.like(out, r.b, r.a)
	case reqCreate:
		c := s.creates[r.a]
		s.discard(r)
		return s.create(out, r.b, c)
	}
	e := errorReplies[r.a]
	return fail(out, e.status, e.msg)
}

func (s *server) health(out []byte) []byte {
	if !dbHealth() {
		b := append(s.json[:0], `{"status":"degraded","db":"unreachable","error":`...)
		b = appendString(b, []byte(dbErr().Error()))
		return appendResponse(out, 503, append(b, '}'))
	}
	b := append(s.json[:0], `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(s.started).Seconds()), 10)
	return appendResponse(out, 200, append(b, '}'))
}

// okHead is the header of a 200 response whose body C writes directly after it. The length
// is not known until the body is there, so its field is left as spaces and the digits are
// written into it afterwards, right-aligned (HTTP allows spaces before a header's value).
const (
	okHead   = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length:       \r\n\r\n"
	okDigits = len(okHead) - 4 // the length's last digit goes just before this index
)

// setLength writes n into the length field of the okHead that starts at out[at].
func setLength(out []byte, at, n int) {
	for i := at + okDigits - 1; ; i-- {
		out[i] = byte('0' + n%10)
		if n /= 10; n == 0 {
			return
		}
	}
}

func (s *server) feed(out []byte) []byte {
	at := len(out)
	out = append(out, okHead...)
	n := -1
	if s.txn() {
		n = dbFeed(out[len(out):cap(out)])
	}
	if n < 0 {
		return internal(out[:at])
	}
	setLength(out, at, n)
	return out[:len(out)+n]
}

func (s *server) getPost(out []byte, id int64) []byte {
	at := len(out)
	out = append(out, okHead...)
	n := -1
	if s.txn() {
		n = dbPost(id, out[len(out):cap(out)])
	}
	switch {
	case n < 0:
		return internal(out[:at])
	case n == 0:
		return fail(out[:at], 404, "post not found")
	}
	setLength(out, at, n)
	return out[:len(out)+n]
}

func (s *server) like(out []byte, uid, id int64) []byte {
	if !s.txn() {
		return internal(out)
	}
	switch dbPostExists(id) {
	case 0:
		return fail(out, 404, "post not found")
	case -1:
		return internal(out)
	}
	inserted := dbLike(uid, id)
	if inserted < 0 {
		return internal(out)
	}
	b := append(s.json[:0], `{"liked":true,"already_liked":`...)
	b = strconv.AppendBool(b, inserted == 0)
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	status := 200
	if inserted == 1 {
		status = 201
	}
	return appendResponse(out, status, append(b, '}'))
}

func (s *server) create(out []byte, uid int64, c createReq) []byte {
	if !s.txn() {
		return internal(out)
	}
	id, created, ok := dbCreate(uid, c.text)
	if !ok {
		return internal(out)
	}
	b := appendPost(append(s.json[:0], `{"post":`...), row{id: id, body: c.text, created: created[:], author: []byte(c.username)})
	return appendResponse(out, 201, append(b, '}'))
}

// auth checks the bearer token. It returns the user's id and name, or the 401 message. The
// name is valid until the next call.
//
// Every token's signature, alg and exp are checked on every request (rule 5). The check
// allocates nothing: the garbage it used to leave behind (about 150 bytes per request) made
// the collector run every 18 seconds under load, and each run paused the loop for up to
// 19 ms, enough to start a burst (measured: two of three bursts in one run began in the second
// of a collection). The two JSON objects of a token are read by a small scanner that knows
// only flat objects of strings and whole numbers; anything else goes to authSlow, the
// standard-library version, so the answers are the same in every case.
func (s *server) auth(head []byte) (uid int64, username []byte, errMsg string) {
	tok, ok := bytes.CutPrefix(header(head, "authorization:"), []byte("Bearer "))
	if !ok {
		return 0, nil, "missing bearer token"
	}
	d1 := bytes.IndexByte(tok, '.')
	d2 := bytes.LastIndexByte(tok, '.')
	if d1 < 0 || d2 <= d1 || bytes.IndexByte(tok[d1+1:d2], '.') >= 0 {
		return 0, nil, "invalid or expired token"
	}
	v := &s.verify
	enc := base64.RawURLEncoding
	if enc.DecodedLen(d1) > len(v.hdr) || enc.DecodedLen(d2-d1-1) > len(v.claims) || enc.DecodedLen(len(tok)-d2-1) > len(v.sig) {
		return s.authSlow(tok)
	}
	nh, err1 := enc.Decode(v.hdr[:], tok[:d1])
	nc, err2 := enc.Decode(v.claims[:], tok[d1+1:d2])
	ns, err3 := enc.Decode(v.sig[:], tok[d2+1:])
	if err1 != nil || err2 != nil || err3 != nil {
		return s.authSlow(tok)
	}
	alg, _, okA := flatField(v.hdr[:nh], "alg")
	if !okA {
		return s.authSlow(tok)
	}
	if string(alg) != "HS256" {
		return 0, nil, "invalid or expired token"
	}
	// HMAC-SHA256 from the two saved states: the hash after the key's inner and outer pad.
	v.h.(encoding.BinaryUnmarshaler).UnmarshalBinary(v.inner)
	v.h.Write(tok[:d2])
	in := v.h.Sum(v.mac[:0])
	v.h.(encoding.BinaryUnmarshaler).UnmarshalBinary(v.outer)
	v.h.Write(in)
	if !hmac.Equal(v.sig[:ns], v.h.Sum(v.mac[:0])) {
		return 0, nil, "invalid or expired token"
	}
	claims := v.claims[:nc]
	expRaw, expIsString, ok1 := flatField(claims, "exp")
	sub, subIsString, ok2 := flatField(claims, "sub")
	name, nameIsString, ok3 := flatField(claims, "username")
	if !ok1 || !ok2 || !ok3 || expIsString || !subIsString || !nameIsString {
		return s.authSlow(tok) // a field missing, of another type, or an object the scanner does not read
	}
	exp, okExp := parseID(expRaw)
	if !okExp {
		return s.authSlow(tok)
	}
	if time.Now().Unix() >= exp {
		return 0, nil, "invalid or expired token"
	}
	uid, idOK := parseID(sub)
	if !idOK {
		return 0, nil, "invalid token payload"
	}
	return uid, name, ""
}

// verifier holds what the token check reuses between requests: buffers, and the SHA-256
// states after hashing the secret's inner and outer pad (the only part of HMAC that is the
// same for every token).
type verifier struct {
	h            hash.Hash
	inner, outer []byte
	mac          [32]byte
	hdr          [256]byte
	claims       [1024]byte
	sig          [64]byte
}

func newVerifier(secret []byte) verifier {
	key := make([]byte, 64)
	if len(secret) > 64 {
		sum := sha256.Sum256(secret)
		copy(key, sum[:])
	} else {
		copy(key, secret)
	}
	state := func(pad byte) []byte {
		h := sha256.New()
		b := make([]byte, 64)
		for i := range b {
			b[i] = key[i] ^ pad
		}
		h.Write(b)
		st, err := h.(encoding.BinaryMarshaler).MarshalBinary()
		if err != nil {
			panic(err)
		}
		return st
	}
	return verifier{h: sha256.New(), inner: state(0x36), outer: state(0x5c)}
}

// flatField finds key in a JSON object whose values are all strings without escapes or
// non-negative whole numbers, and returns the value's bytes (a string without its quotes).
// ok is false if the key is absent or the object is anything this scanner does not read, in
// which case the caller uses the standard decoder instead.
func flatField(obj []byte, key string) (val []byte, isString, ok bool) {
	i := skipSpace(obj, 0)
	if i >= len(obj) || obj[i] != '{' {
		return nil, false, false
	}
	i = skipSpace(obj, i+1)
	if i < len(obj) && obj[i] == '}' {
		return nil, false, false
	}
	for {
		k, next, kok := flatString(obj, i)
		if !kok {
			return nil, false, false
		}
		i = skipSpace(obj, next)
		if i >= len(obj) || obj[i] != ':' {
			return nil, false, false
		}
		i = skipSpace(obj, i+1)
		var v []byte
		str := false
		if i < len(obj) && obj[i] == '"' {
			var vok bool
			if v, next, vok = flatString(obj, i); !vok {
				return nil, false, false
			}
			str = true
		} else {
			next = i
			for next < len(obj) && obj[next] >= '0' && obj[next] <= '9' {
				next++
			}
			if next == i {
				return nil, false, false
			}
			v = obj[i:next]
		}
		switch found := string(k) == key; {
		case found && !ok:
			val, isString, ok = v, str, true
		case found, strings.EqualFold(string(k), key):
			// A repeated key, or the same key in another case (which the standard decoder
			// also matches when it fills a struct): let the standard decoder decide.
			return nil, false, false
		}
		i = skipSpace(obj, next)
		if i >= len(obj) {
			return nil, false, false
		}
		if obj[i] == '}' {
			if skipSpace(obj, i+1) != len(obj) {
				return nil, false, false
			}
			return val, isString, ok
		}
		if obj[i] != ',' {
			return nil, false, false
		}
		i = skipSpace(obj, i+1)
	}
}

// flatString reads a JSON string with no escapes and no control characters starting at the
// quote at b[i]. It returns the contents and the index after the closing quote.
func flatString(b []byte, i int) (s []byte, next int, ok bool) {
	if i >= len(b) || b[i] != '"' {
		return nil, 0, false
	}
	for j := i + 1; j < len(b); j++ {
		switch c := b[j]; {
		case c == '"':
			return b[i+1 : j], j + 1, utf8.Valid(b[i+1 : j])
		case c == '\\' || c < 0x20:
			return nil, 0, false
		}
	}
	return nil, 0, false
}

func skipSpace(b []byte, i int) int {
	for i < len(b) && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') {
		i++
	}
	return i
}

// authSlow is the token check with the standard library, for tokens the fast path does not
// read. It gives the same answers; it allocates.
func (s *server) authSlow(tok []byte) (uid int64, username []byte, errMsg string) {
	parts := strings.Split(string(tok), ".")
	if len(parts) != 3 {
		return 0, nil, "invalid or expired token"
	}
	var hdr struct {
		Alg string `json:"alg"`
	}
	if !decodeSegment(parts[0], &hdr) || hdr.Alg != "HS256" {
		return 0, nil, "invalid or expired token"
	}
	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return 0, nil, "invalid or expired token"
	}
	mac := hmac.New(sha256.New, s.secret)
	mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(sig, mac.Sum(nil)) {
		return 0, nil, "invalid or expired token"
	}
	var claims map[string]any
	if !decodeSegment(parts[1], &claims) {
		return 0, nil, "invalid or expired token"
	}
	if exp, ok := claims["exp"].(float64); !ok || float64(time.Now().Unix()) >= exp {
		return 0, nil, "invalid or expired token"
	}
	sub, _ := claims["sub"].(string)
	name, nameOK := claims["username"].(string)
	uid, idOK := parseID([]byte(sub))
	if !idOK || !nameOK {
		return 0, nil, "invalid token payload"
	}
	return uid, []byte(name), ""
}

func decodeSegment(seg string, v any) bool {
	b, err := base64.RawURLEncoding.DecodeString(seg)
	return err == nil && json.Unmarshal(b, v) == nil
}

// parseID accepts only a positive base-10 integer that fits in an int64: no sign, no
// leading zero, no fraction, no spaces.
func parseID(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 19 || b[0] < '1' || b[0] > '9' {
		return 0, false
	}
	var n uint64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + uint64(c-'0')
	}
	return int64(n), n <= 1<<63-1
}

// header returns the value of the named header (name is lower-case and ends with ':'),
// or nil. Header names are matched case-insensitively.
func header(head []byte, name string) []byte {
	// From line to line: IndexByte finds the next line end many bytes at a time, and only
	// the start of each line is compared with the name. (This walked the head byte by byte,
	// twice per request, and was 2 % of the server's instructions.)
	for {
		i := bytes.IndexByte(head, '\n')
		if i < 0 || len(head)-i-1 < len(name) {
			return nil
		}
		head = head[i+1:]
		j := 0
		for j < len(name) && head[j]|0x20 == name[j] {
			j++
		}
		if j < len(name) {
			continue
		}
		v := head[j:]
		if end := bytes.IndexByte(v, '\r'); end >= 0 {
			v = v[:end]
		}
		return bytes.TrimSpace(v)
	}
}

func contentLength(head []byte) int {
	n := 0
	for _, c := range header(head, "content-length:") {
		if c < '0' || c > '9' {
			return 0
		}
		n = n*10 + int(c-'0')
	}
	return n
}

// appendPost writes one post object with the keys in the order the spec fixes.
func appendPost(b []byte, r row) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, r.id, 10)
	b = append(b, `,"body":`...)
	b = appendString(b, r.body)
	b = append(b, `,"created_at":`...)
	b = appendString(b, r.created)
	b = append(b, `,"author":`...)
	b = appendString(b, r.author)
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, r.likes, 10)
	return append(b, '}')
}

const hexDigits = "0123456789abcdef"

// appendString writes s as a JSON string. Only what JSON requires is escaped: the quote,
// the backslash and control characters. Everything else, including UTF-8, passes through.
func appendString(b, s []byte) []byte {
	b = append(b, '"')
	for i := 0; ; {
		j := firstEscape(s, i)
		b = append(b, s[i:j]...)
		if j == len(s) {
			return append(b, '"')
		}
		switch c := s[j]; c {
		case '"', '\\':
			b = append(b, '\\', c)
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		i = j + 1
	}
}

// firstEscape returns the index of the first byte at or after i that JSON must escape, or
// len(s). It looks at eight bytes at a time: nearly every post body and name needs no
// escaping at all, and a byte-by-byte loop over them was 3 % of all CPU in a profile.
func firstEscape(s []byte, i int) int {
	const ones, highs = 0x0101010101010101, 0x8080808080808080
	for ; i+8 <= len(s); i += 8 {
		w := binary.LittleEndian.Uint64(s[i:])
		q, bs := w^(ones*'"'), w^(ones*'\\')
		// For each byte: the high bit is set if it is below 0x20, a quote or a backslash.
		// (Bytes above the first match can be flagged wrongly; only the first match is used.)
		if m := ((w - ones*0x20) &^ w & highs) | ((q - ones) &^ q & highs) | ((bs - ones) &^ bs & highs); m != 0 {
			return i + bits.TrailingZeros64(m)/8
		}
	}
	for ; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c == '"' || c == '\\' {
			return i
		}
	}
	return len(s)
}

var statusLine = map[int]string{
	200: "200 OK", 201: "201 Created", 400: "400 Bad Request", 401: "401 Unauthorized",
	404: "404 Not Found", 500: "500 Internal Server Error", 503: "503 Service Unavailable",
}

func appendResponse(out []byte, status int, body []byte) []byte {
	out = append(out, "HTTP/1.1 "...)
	out = append(out, statusLine[status]...)
	out = append(out, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	out = strconv.AppendInt(out, int64(len(body)), 10)
	out = append(out, "\r\n\r\n"...)
	return append(out, body...)
}

func fail(out []byte, status int, msg string) []byte {
	b := append(make([]byte, 0, 64), `{"error":"`...)
	b = append(b, msg...)
	return appendResponse(out, status, append(b, `"}`...))
}

func internal(out []byte) []byte {
	log.Printf("internal: %v", dbErr())
	return fail(out, 500, "internal server error")
}
