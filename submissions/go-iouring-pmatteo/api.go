package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
	"time"
)

// Every error body is a constant: no formatting on the error paths.
const (
	errNotFound     = `{"error":"not found"}`
	errInvalidID    = `{"error":"invalid post id"}`
	errPostNotFound = `{"error":"post not found"}`
	errNoBearer     = `{"error":"missing bearer token"}`
	errBadToken     = `{"error":"invalid or expired token"}`
	errBadPayload   = `{"error":"invalid token payload"}`
	errBadJSON      = `{"error":"malformed JSON body"}`
	errBodyRequired = `{"error":"body is required"}`
	errBodyTooLong  = `{"error":"body must be at most 500 characters"}`
	errInternal     = `{"error":"internal server error"}`
)

const maxBodyChars = 500

type request struct {
	method []byte
	path   []byte
	auth   []byte // the Authorization header value, nil when the header is absent
	body   []byte
}

// server is the whole serving state. One instance, entered from the loop thread only.
type server struct {
	db      *db
	ver     *verifier
	started time.Time
	trim    []byte // scratch for the unescaped, trimmed POST body

	feedIDs    [feedLimit]int64 // the newest ids, in output order
	likeCounts []int32          // counts indexed by id - lo, for one feed window
}

const (
	feedLimit = 20
	// maxFeedSpan bounds the id window the range scan will cover. Twenty contiguous posts span
	// twenty ids; anything much wider means the newest posts are not clustered and the range
	// scan would read rows it throws away, so the reference query takes over.
	maxFeedSpan = 1024
)

// The response bound. maxRespBytes is what the loop sizes a response buffer from, and it is
// derived from the largest body the spec permits: a /feed of feedLimit posts.
//
// It must not be derived from maxBodyBytes, the inbound Content-Length ceiling. It was, and the
// two have nothing to do with each other: twenty posts of maxBodyChars characters render to
// about 42 KB against an 8 KB buffer, so a legal feed overflowed, grew onto the Go heap, failed
// the copy back into the arena and answered 500. No benchmark saw it because the seed bodies
// average 130 characters.
const (
	// A character is up to 4 bytes in UTF-8, and up to 6 once JSON-escaped (\u00XX for a
	// control byte), so the escaped worst case is 6 per character of the 500 the schema allows.
	maxBodyEscaped = maxBodyChars * 6
	// users.username carries no CHECK, so this is a documented bound rather than a derived one:
	// a username longer than it is the one case that still falls to the counted overflow path
	// in engine.serve.
	maxAuthorBytes  = 256
	maxCreatedBytes = 32
	// id, like_count, the five keys and the punctuation.
	maxPostEnvelope = 96
	maxPostBytes    = maxPostEnvelope + maxBodyEscaped + maxAuthorBytes + maxCreatedBytes
	// feedLimit posts, their separating commas, and {"posts":[ ]}.
	maxRespBytes = 16 + feedLimit*(maxPostBytes+1)
)

// handle renders the response body onto out and returns the status. out is the caller's buffer,
// so handle allocates only when that buffer has to grow.
//
// r is a pointer because request is four slice headers, 96 bytes: by value it was copied into
// every routed handler and again into authenticate and authError. Nothing below retains it.
func (s *server) handle(r *request, out []byte) (int, []byte) {
	switch {
	case eq(r.method, "GET") && eq(r.path, "/health"):
		return s.health(out)
	case eq(r.method, "GET") && eq(r.path, "/feed"):
		return s.feed(out)
	case eq(r.method, "GET") && hasPrefix(r.path, "/posts/"):
		return s.getPost(r, out)
	case eq(r.method, "POST") && eq(r.path, "/posts"):
		return s.createPost(r, out)
	// The length test is not redundant: "/posts/like" matches both affixes on overlapping bytes,
	// leaving a negative-length id segment. No id means this is not the route.
	case eq(r.method, "POST") && hasPrefix(r.path, "/posts/") && hasSuffix(r.path, "/like") &&
		len(r.path) > len("/posts/")+len("/like"):
		return s.likePost(r, out)
	}
	return 404, append(out, errNotFound...)
}

func (s *server) health(out []byte) (int, []byte) {
	if !s.db.pingOK() {
		return 503, append(out, `{"status":"degraded","db":"unreachable","error":"SELECT 1 failed"}`...)
	}
	out = append(out, `{"status":"ok","db":"ok","uptime_s":`...)
	out = appendInt(out, int64(time.Since(s.started)/time.Second))
	return 200, append(out, '}')
}

func (s *server) feed(out []byte) (int, []byte) {
	if status, body, ok := s.feedByRange(out); ok {
		return status, body
	}
	return s.feedByScan(out)
}

// feedByRange answers the feed from three range scans. The third return is false when the data
// does not suit it, which leaves out untouched and sends the caller to the reference query.
func (s *server) feedByRange(out []byte) (int, []byte, bool) {
	ids := s.feedIDs[:0]
	st := s.db.feedIDs
	for {
		row, err := s.db.step(st)
		if err != nil {
			reset(st)
			return 500, append(out, errInternal...), true
		}
		if !row {
			break
		}
		ids = append(ids, colInt(st, 0))
	}
	reset(st)

	// Contiguous and strictly descending by id is what makes one backwards rowid scan return
	// exactly these rows in exactly this order.
	if len(ids) == 0 {
		return 0, out, false
	}
	for i := 1; i < len(ids); i++ {
		if ids[i] >= ids[i-1] {
			return 0, out, false
		}
	}
	lo, hi := ids[len(ids)-1], ids[0]
	span := hi - lo + 1
	if span > maxFeedSpan {
		return 0, out, false
	}

	counts := s.likeCounts[:span]
	for i := range counts {
		counts[i] = 0
	}
	lk := s.db.likesRange
	bindInt(lk, 1, lo)
	bindInt(lk, 2, hi)
	for {
		row, err := s.db.step(lk)
		if err != nil {
			reset(lk)
			return 500, append(out, errInternal...), true
		}
		if !row {
			break
		}
		counts[colInt(lk, 0)-lo] = int32(colInt(lk, 1))
	}
	reset(lk)

	pr := s.db.feedRange
	defer reset(pr)
	bindInt(pr, 1, lo)
	bindInt(pr, 2, hi)
	out = append(out, `{"posts":[`...)
	k := 0
	for k < len(ids) {
		row, err := s.db.step(pr)
		if err != nil {
			return 500, append(out[:0], errInternal...), true
		}
		if !row {
			break
		}
		id := colInt(pr, 0)
		if id != ids[k] {
			continue // in the id window but not in the newest twenty
		}
		if k > 0 {
			out = append(out, ',')
		}
		out = appendRangePost(out, pr, id, counts[id-lo])
		k++
	}
	if k != len(ids) {
		// The window did not yield every post it promised. Nothing can cause this on a single
		// thread, so rather than serve a short feed, fall back.
		return 0, out[:0], false
	}
	return 200, append(out, ']', '}'), true
}

func (s *server) feedByScan(out []byte) (int, []byte) {
	st := s.db.feed
	defer reset(st)

	out = append(out, `{"posts":[`...)
	for i := 0; ; i++ {
		row, err := s.db.step(st)
		if err != nil {
			return 500, append(out[:0], errInternal...)
		}
		if !row {
			break
		}
		if i > 0 {
			out = append(out, ',')
		}
		out = appendPost(out, st)
	}
	return 200, append(out, ']', '}')
}

func (s *server) getPost(r *request, out []byte) (int, []byte) {
	id, ok := parsePositiveInt(r.path[len("/posts/"):])
	if !ok {
		return 400, append(out, errInvalidID...)
	}
	st := s.db.post
	defer reset(st)
	bindInt(st, 1, id)
	row, err := s.db.step(st)
	if err != nil {
		return 500, append(out, errInternal...)
	}
	if !row {
		return 404, append(out, errPostNotFound...)
	}
	out = append(out, `{"post":`...)
	out = appendPost(out, st)
	return 200, append(out, '}')
}

func (s *server) createPost(r *request, out []byte) (int, []byte) {
	c, res := s.authenticate(r)
	if res != authOK {
		return authError(out, r, res)
	}
	if !jsonValid(r.body) {
		return 400, append(out, errBadJSON...)
	}
	raw, kind := jsonField(r.body, "body")
	if kind != jsonString {
		return 400, append(out, errBodyRequired...)
	}
	unescaped, ok := jsonUnescape(s.trim[:0], raw)
	s.trim = unescaped
	if !ok {
		return 400, append(out, errBadJSON...)
	}
	body := trimSpace(unescaped)
	if len(body) == 0 {
		return 400, append(out, errBodyRequired...)
	}
	// The limit is characters, not bytes: posts.body carries CHECK (length(body) BETWEEN 1 AND
	// 500), and length() counts characters, so a byte count would turn a long multibyte body
	// into a 500 instead of a 400.
	// A UTF-8 string never has more characters than bytes, so the scan is only needed when
	// the byte count alone cannot settle it.
	if len(body) > maxBodyChars && runeCount(body) > maxBodyChars {
		return 400, append(out, errBodyTooLong...)
	}

	st := s.db.insertPost
	defer reset(st)
	bindInt(st, 1, c.userID)
	bindBody(st, 2, body)
	row, err := s.db.step(st)
	if err != nil || !row {
		return 500, append(out, errInternal...)
	}
	id := colInt(st, 0)
	createdAt := colText(st, 1)

	out = append(out, `{"post":{"id":`...)
	out = appendInt(out, id)
	out = append(out, `,"body":`...)
	out = appendJSONString(out, body)
	out = append(out, `,"created_at":`...)
	out = appendJSONString(out, createdAt)
	out = append(out, `,"author":`...)
	// The author of a new post is the token's username, not a users row (rule: it is not looked
	// up). c.username stays valid here because only verify() writes that buffer.
	out = appendJSONString(out, c.username)
	out = append(out, `,"like_count":0}}`...)
	return 201, out
}

func (s *server) likePost(r *request, out []byte) (int, []byte) {
	c, res := s.authenticate(r)
	if res != authOK {
		return authError(out, r, res)
	}
	seg := r.path[len("/posts/") : len(r.path)-len("/like")]
	id, ok := parsePositiveInt(seg)
	if !ok {
		return 400, append(out, errInvalidID...)
	}

	// Foreign keys are off, so an insert against a missing post would succeed and answer 201
	// where the spec wants 404. The existence check is what makes the difference.
	ex := s.db.postExists
	bindInt(ex, 1, id)
	row, err := s.db.step(ex)
	reset(ex)
	if err != nil {
		return 500, append(out, errInternal...)
	}
	if !row {
		return 404, append(out, errPostNotFound...)
	}

	st := s.db.insertLike
	bindInt(st, 1, c.userID)
	bindInt(st, 2, id)
	if _, err := s.db.step(st); err != nil {
		reset(st)
		return 500, append(out, errInternal...)
	}
	inserted := s.db.changes() == 1
	reset(st)

	status := 200
	alreadyLiked := `true`
	if inserted {
		status, alreadyLiked = 201, `false`
	}
	out = append(out, `{"liked":true,"already_liked":`...)
	out = append(out, alreadyLiked...)
	out = append(out, `,"post_id":`...)
	out = appendInt(out, id)
	return status, append(out, '}')
}

// authenticate pulls the bearer token out of the Authorization header and verifies it.
func (s *server) authenticate(r *request) (claims, authResult) {
	const prefix = "Bearer "
	if !hasPrefix(r.auth, prefix) {
		return claims{}, authInvalid // distinguished from a bad token by authError
	}
	return s.ver.verify(r.auth[len(prefix):], time.Now().Unix())
}

// authError maps a failed authentication to its body. A missing or non-bearer header is
// "missing bearer token"; anything else is about the token itself.
func authError(out []byte, r *request, res authResult) (int, []byte) {
	const prefix = "Bearer "
	if !hasPrefix(r.auth, prefix) {
		return 401, append(out, errNoBearer...)
	}
	if res == authBadPayload {
		return 401, append(out, errBadPayload...)
	}
	return 401, append(out, errBadToken...)
}

// appendRangePost renders one post from the range-scan statement, whose columns stop at the
// author because the like count came from the counting pass. id is passed in because the caller
// has already read it to match the row, and reading it again is another cgo crossing.
func appendRangePost(out []byte, st stmt, id int64, likes int32) []byte {
	out = append(out, `{"id":`...)
	out = appendInt(out, id)
	out = append(out, `,"body":`...)
	out = appendJSONString(out, colText(st, 1))
	out = append(out, `,"created_at":`...)
	out = appendJSONString(out, colText(st, 2))
	out = append(out, `,"author":`...)
	out = appendJSONString(out, colText(st, 3))
	out = append(out, `,"like_count":`...)
	out = appendInt(out, int64(likes))
	return append(out, '}')
}

// appendPost renders one post object from the current row. The column order is fixed by the
// statements in db.go: id, body, created_at, username, like_count.
func appendPost(out []byte, st stmt) []byte {
	out = append(out, `{"id":`...)
	out = appendInt(out, colInt(st, 0))
	out = append(out, `,"body":`...)
	out = appendJSONString(out, colText(st, 1))
	out = append(out, `,"created_at":`...)
	out = appendJSONString(out, colText(st, 2))
	out = append(out, `,"author":`...)
	out = appendJSONString(out, colText(st, 3))
	out = append(out, `,"like_count":`...)
	out = appendInt(out, colInt(st, 4))
	return append(out, '}')
}

// ---- from jwt.go ---------------------------------------------------------------------------------------

type authResult uint8

const (
	authOK         authResult = iota
	authInvalid               // bad signature, wrong alg, expired, malformed: "invalid or expired token"
	authBadPayload            // valid signature, unusable claims: "invalid token payload"
)

// verifier verifies HS256 tokens. Nothing about a token is remembered between requests (rule 5):
// every signature is recomputed. The buffers are reused, the hasher is reset, so a verification
// allocates nothing.
type verifier struct {
	mac     hash.Hash
	header  []byte
	payload []byte
	sig     []byte
	sum     []byte
	scratch []byte
}

func newVerifier(secret []byte) *verifier {
	return &verifier{
		mac:     hmac.New(sha256.New, secret),
		header:  make([]byte, 0, 256),
		payload: make([]byte, 0, 1024),
		sig:     make([]byte, 0, 64),
		sum:     make([]byte, 0, sha256.Size),
		scratch: make([]byte, 0, 256),
	}
}

type claims struct {
	userID   int64
	username []byte // a view into v.scratch, valid until the next verify
}

// verify checks the token and extracts the claims. The checks run in the order test.sh asserts:
// alg, signature and exp first (authInvalid), then the claim shapes (authBadPayload).
func (v *verifier) verify(token []byte, nowUnix int64) (claims, authResult) {
	var c claims

	dot1 := bytes.IndexByte(token, '.')
	if dot1 < 0 {
		return c, authInvalid
	}
	dot2 := bytes.IndexByte(token[dot1+1:], '.')
	if dot2 < 0 {
		return c, authInvalid
	}
	dot2 += dot1 + 1
	if bytes.IndexByte(token[dot2+1:], '.') >= 0 {
		return c, authInvalid
	}

	var ok bool
	if v.header, ok = decodeB64(v.header[:0], token[:dot1]); !ok {
		return c, authInvalid
	}
	alg, kind := jsonField(v.header, "alg")
	if kind != jsonString || string(alg) != "HS256" {
		return c, authInvalid
	}

	if v.sig, ok = decodeB64(v.sig[:0], token[dot2+1:]); !ok {
		return c, authInvalid
	}
	v.mac.Reset()
	v.mac.Write(token[:dot2])
	v.sum = v.mac.Sum(v.sum[:0])
	if !hmac.Equal(v.sum, v.sig) {
		return c, authInvalid
	}

	if v.payload, ok = decodeB64(v.payload[:0], token[dot1+1:dot2]); !ok {
		return c, authInvalid
	}

	// exp is mandatory: a token without one is rejected outright.
	expRaw, kind := jsonField(v.payload, "exp")
	if kind != jsonNumber {
		return c, authInvalid
	}
	exp, ok := parseSeconds(expRaw)
	if !ok || exp <= nowUnix {
		return c, authInvalid
	}

	subRaw, kind := jsonField(v.payload, "sub")
	if kind != jsonString {
		return c, authBadPayload
	}
	id, ok := parsePositiveInt(subRaw)
	if !ok {
		return c, authBadPayload
	}

	nameRaw, kind := jsonField(v.payload, "username")
	if kind != jsonString {
		return c, authBadPayload
	}
	name, ok := jsonUnescape(v.scratch[:0], nameRaw)
	if !ok {
		return c, authBadPayload
	}
	v.scratch = name

	c.userID = id
	c.username = name
	return c, authOK
}

// decodeB64 decodes unpadded base64url. Tokens in the wild sometimes carry padding, so both are
// accepted.
func decodeB64(dst, src []byte) ([]byte, bool) {
	for len(src) > 0 && src[len(src)-1] == '=' {
		src = src[:len(src)-1]
	}
	need := base64.RawURLEncoding.DecodedLen(len(src))
	if cap(dst) < need {
		dst = make([]byte, 0, need*2)
	}
	dst = dst[:need]
	n, err := base64.RawURLEncoding.Decode(dst, src)
	if err != nil {
		return dst[:0], false
	}
	return dst[:n], true
}

// parseSeconds reads an integer-or-fractional epoch value, truncating any fraction.
func parseSeconds(b []byte) (int64, bool) {
	if i := bytes.IndexByte(b, '.'); i >= 0 {
		b = b[:i]
	}
	neg := false
	if len(b) > 0 && b[0] == '-' {
		neg, b = true, b[1:]
	}
	if len(b) == 0 || len(b) > 19 {
		return 0, false
	}
	var v int64
	for _, c := range b {
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

// parsePositiveInt accepts the decimal digits of a positive integer and nothing else: no sign,
// no leading zero, no whitespace. This is the rule for a JWT "sub" and for a ":id" path segment.
func parsePositiveInt(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 18 || b[0] == '0' {
		return 0, false
	}
	var v int64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, true
}
