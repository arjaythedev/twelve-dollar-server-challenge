package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"hash"
	"math"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
	"unsafe"
)

const (
	postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id`
	feedSQL       = postSelect + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20`
	postSQL       = postSelect + ` WHERE p.id = ?1`
	insertPostSQL = `INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at`
	// Inserts only if the post exists; no change means "already liked" or "no such post".
	insertLikeSQL = `INSERT INTO likes (user_id, post_id)
SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
ON CONFLICT (user_id, post_id) DO NOTHING`
	postExistsSQL = `SELECT 1 FROM posts WHERE id = ?1`

	// The feed as three range scans (see getFeed).
	feedIDsSQL   = `SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20`
	feedRowsSQL  = `SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2`
	feedLikesSQL = `SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id`
	feedSpan     = 64 // the range scans run only when the 20 newest ids are this close together
)

var started = time.Now()

// api is the request state: the SQLite connection with its prepared statements, and the JWT key.
// Nothing in here remembers data between requests (rule 5).
type api struct {
	db                                         *db
	health, feed, post, insertPost, insertLike *stmt
	postExists, feedIDs, feedRows, feedLikes   *stmt
	mac                                        hash.Hash
	sum                                        []byte
	jwt                                        []byte // decoded JWT parts
	top                                        [20]int64
	span                                       [feedSpan]feedRow
	text                                       []byte // the feed rows' text, copied out of SQLite
}

// feedRow is one post of a feed range scan. Its body, created_at and author are
// text[t[0]:t[1]], text[t[1]:t[2]] and text[t[2]:t[3]].
type feedRow struct {
	found bool
	t     [4]int32
	likes int64
}

func newAPI(path string, secret []byte) (api, error) {
	d, err := openDB(path)
	if err != nil {
		return api{}, err
	}
	for _, pragma := range []string{
		// This process is the only user of the database, and one connection serves every request.
		// Exclusive locking keeps the WAL index in heap memory instead of the -shm file, so a
		// query takes no file locks: 17% less CPU per request in a 1-core run. It must come
		// before anything reads the database.
		"PRAGMA locking_mode = EXCLUSIVE",
		"PRAGMA journal_mode = WAL",
		"PRAGMA synchronous = NORMAL",   // rule 6: WAL + NORMAL
		"PRAGMA busy_timeout = 5000", // in case another process has the file open at startup
		"PRAGMA cache_size = -16384", // 16 MiB. No mmap: it measured 6% slower per request
		"PRAGMA temp_store = MEMORY",
		// Checkpoint every 10,000 WAL pages (40 MB) instead of 1,000. A checkpoint copies each
		// changed page once however often it changed, and fsyncs twice on the event loop, so
		// fewer of them cost less: each like or post blocks the loop for half as long.
		"PRAGMA wal_autocheckpoint = 10000",
	} {
		if err := d.exec(pragma); err != nil {
			return api{}, err
		}
	}
	a := api{db: d, mac: hmac.New(sha256.New, secret)}
	for _, s := range []struct {
		p    **stmt
		sql  string
		text []int
	}{
		{&a.health, "SELECT 1", nil},
		{&a.feed, feedSQL, []int{1, 2, 3}},
		{&a.post, postSQL, []int{1, 2, 3}},
		{&a.insertPost, insertPostSQL, []int{1}},
		{&a.insertLike, insertLikeSQL, nil},
		{&a.postExists, postExistsSQL, nil},
		{&a.feedIDs, feedIDsSQL, nil},
		{&a.feedRows, feedRowsSQL, []int{1, 2, 3}},
		{&a.feedLikes, feedLikesSQL, nil},
	} {
		if *s.p, err = d.prepare(s.sql, s.text...); err != nil {
			return api{}, err
		}
	}
	return a, nil
}

// handle routes one request and appends its response to l.out.
func (l *loop) handle(r *request) {
	get, post := string(r.method) == "GET", string(r.method) == "POST"
	switch path := r.path; {
	case string(path) == "/feed":
		if get {
			l.getFeed(r)
			return
		}
	case string(path) == "/posts":
		if post {
			l.createPost(r)
			return
		}
	case len(path) > 7 && string(path[:7]) == "/posts/":
		id := path[7:]
		slash := bytes.IndexByte(id, '/')
		if slash < 0 {
			if get {
				l.getPost(r, id)
				return
			}
		} else if slash > 0 && string(id[slash:]) == "/like" {
			if post {
				l.like(r, id[:slash])
				return
			}
		} else {
			l.fail(r, 404, "not found")
			return
		}
	case string(path) == "/health":
		if get {
			l.getHealth(r)
			return
		}
	default:
		l.fail(r, 404, "not found")
		return
	}
	l.fail(r, 405, "method not allowed")
}

func (l *loop) fail(r *request, code int, msg string) {
	l.body = appendError(l.body[:0], msg)
	l.respond(r, code, l.body)
}

func (l *loop) getHealth(r *request) {
	_, err := l.health.step()
	l.health.reset()
	b := l.body[:0]
	if err != nil {
		b = append(b, `{"status":"degraded","db":"unreachable","error":`...)
		b = appendString(b, err.Error())
		l.body = append(b, '}')
		l.respond(r, 503, l.body)
		return
	}
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(started)/time.Second), 10)
	l.body = append(b, '}')
	l.respond(r, 200, l.body)
}

// getFeed answers with the 20 newest posts. Ids grow with created_at (both are assigned at
// insert), so the 20 newest posts almost always have adjacent ids, and three range scans can do
// the work of the reference query's 60 lookups (20 each for the post, its author and its like
// count): the ids from the created_at index, then the posts with their authors in [lo, hi], then
// the like counts in [lo, hi] from likes_post_id_idx. That's a third less CPU per feed. If the
// ids are far apart or a post has no author row, the reference query answers instead. The three
// scans see the same data: this connection is the only writer, and nothing runs in between.
func (l *loop) getFeed(r *request) {
	ids := l.top[:0]
	s := l.feedIDs
	for {
		ok, err := s.step()
		if err != nil {
			s.reset()
			l.fail(r, 500, "internal server error")
			return
		}
		if !ok {
			break
		}
		ids = append(ids, s.int64(0))
	}
	s.reset()
	if len(ids) == 0 {
		l.getFeedReference(r)
		return
	}
	lo, hi := slices.Min(ids), slices.Max(ids)
	if hi-lo >= feedSpan {
		l.getFeedReference(r)
		return
	}
	rows := l.span[:hi-lo+1]
	clear(rows)
	text := l.text[:0]
	s = l.feedRows
	s.bindInt(1, lo)
	s.bindInt(2, hi)
	for {
		ok, err := s.step()
		if err != nil {
			s.reset()
			l.fail(r, 500, "internal server error")
			return
		}
		if !ok {
			break
		}
		row := &rows[s.int64(0)-lo]
		row.found = true
		row.t[0] = int32(len(text))
		for c := 1; c <= 3; c++ {
			text = append(text, s.text(c)...)
			row.t[c] = int32(len(text))
		}
	}
	s.reset()
	l.text = text
	s = l.feedLikes
	s.bindInt(1, lo)
	s.bindInt(2, hi)
	for {
		ok, err := s.step()
		if err != nil {
			s.reset()
			l.fail(r, 500, "internal server error")
			return
		}
		if !ok {
			break
		}
		rows[s.int64(0)-lo].likes = s.int64(1)
	}
	s.reset()
	b := append(l.body[:0], `{"posts":[`...)
	for i, id := range ids {
		row := &rows[id-lo]
		if !row.found {
			l.getFeedReference(r) // a post without an author row: the JOIN drops it
			return
		}
		if i > 0 {
			b = append(b, ',')
		}
		t := row.t
		b = appendPostFields(b, id, view(text[t[0]:t[1]]), view(text[t[1]:t[2]]), view(text[t[2]:t[3]]), row.likes)
	}
	l.body = append(b, "]}"...)
	l.respond(r, 200, l.body)
}

// getFeedReference is the feed by the reference query.
func (l *loop) getFeedReference(r *request) {
	s := l.feed
	b := append(l.body[:0], `{"posts":[`...)
	for i := 0; ; i++ {
		ok, err := s.step()
		if err != nil {
			s.reset()
			l.fail(r, 500, "internal server error")
			return
		}
		if !ok {
			break
		}
		if i > 0 {
			b = append(b, ',')
		}
		b = appendPost(b, s)
	}
	s.reset()
	l.body = append(b, "]}"...)
	l.respond(r, 200, l.body)
}

func (l *loop) getPost(r *request, rawID []byte) {
	id, ok := parseID(rawID)
	if !ok {
		l.fail(r, 400, "invalid post id")
		return
	}
	s := l.post
	s.bindInt(1, id)
	found, err := s.step()
	if err != nil || !found {
		s.reset()
		if err != nil {
			l.fail(r, 500, "internal server error")
		} else {
			l.fail(r, 404, "post not found")
		}
		return
	}
	b := append(l.body[:0], `{"post":`...)
	b = appendPost(b, s)
	s.reset()
	l.body = append(b, '}')
	l.respond(r, 200, l.body)
}

func (l *loop) createPost(r *request) {
	uid, username, msg := l.authenticate(r.auth)
	if msg != "" {
		l.fail(r, 401, msg)
		return
	}
	text, msg := postBody(r.body)
	if msg != "" {
		l.fail(r, 400, msg)
		return
	}
	s := l.insertPost
	s.bindInt(1, uid)
	s.bindText(2, text)
	ok, err := s.step()
	if err != nil || !ok {
		s.reset()
		l.fail(r, 500, "internal server error")
		return
	}
	b := append(l.body[:0], `{"post":{"id":`...)
	b = strconv.AppendInt(b, s.int64(0), 10)
	b = append(b, `,"body":`...)
	b = appendString(b, text)
	b = append(b, `,"created_at":`...)
	b = appendString(b, s.text(1))
	b = append(b, `,"author":`...)
	b = appendString(b, username)
	l.body = append(b, `,"like_count":0}}`...)
	// The autocommit transaction commits when the statement runs to completion: only then is
	// the row durable and a 201 allowed (rule 6).
	more, err := s.step()
	s.reset()
	if err != nil || more {
		l.fail(r, 500, "internal server error")
		return
	}
	l.respond(r, 201, l.body)
}

func (l *loop) like(r *request, rawID []byte) {
	uid, _, msg := l.authenticate(r.auth) // auth before the id, per the spec
	if msg != "" {
		l.fail(r, 401, msg)
		return
	}
	id, ok := parseID(rawID)
	if !ok {
		l.fail(r, 400, "invalid post id")
		return
	}
	s := l.insertLike
	s.bindInt(1, uid)
	s.bindInt(2, id)
	_, err := s.step()
	s.reset()
	if err != nil {
		l.fail(r, 500, "internal server error")
		return
	}
	code, already := 201, false
	if l.db.changes() == 0 {
		s = l.postExists
		s.bindInt(1, id)
		exists, err := s.step()
		s.reset()
		if err != nil {
			l.fail(r, 500, "internal server error")
			return
		}
		if !exists {
			l.fail(r, 404, "post not found")
			return
		}
		code, already = 200, true
	}
	b := append(l.body[:0], `{"liked":true,"already_liked":`...)
	b = strconv.AppendBool(b, already)
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	l.body = append(b, '}')
	l.respond(r, code, l.body)
}

// parseID accepts plain decimal digits with a value above zero. An id beyond SQLite's INTEGER
// range is valid but can't exist, so it comes back as 0 with ok=true: a lookup finds nothing.
func parseID[T string | []byte](s T) (id int64, ok bool) {
	if len(s) == 0 {
		return 0, false
	}
	overflow := false
	for i := range len(s) {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		d := int64(c - '0')
		if id > (math.MaxInt64-d)/10 {
			overflow = true
		} else {
			id = id*10 + d
		}
	}
	if overflow {
		return 0, true
	}
	return id, id > 0
}

// postBody validates a POST /posts body and returns the trimmed text, or an error message.
func postBody(raw []byte) (string, string) {
	if !json.Valid(raw) {
		return "", "malformed JSON body"
	}
	var fields map[string]json.RawMessage
	var text string
	if json.Unmarshal(raw, &fields) != nil { // valid JSON, but not an object
		return "", "body is required"
	}
	if v := fields["body"]; len(v) == 0 || v[0] != '"' || json.Unmarshal(v, &text) != nil {
		return "", "body is required"
	}
	if text = strings.TrimFunc(text, isJSSpace); text == "" {
		return "", "body is required"
	}
	if utf8.RuneCountInString(text) > 500 {
		return "", "body must be at most 500 characters"
	}
	return text, ""
}

// isJSSpace is JavaScript's String.prototype.trim() set (WhiteSpace and LineTerminator), so
// bodies are trimmed like the reference implementations.
func isJSSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

var b64 = base64.RawURLEncoding

// authenticate verifies an "Authorization: Bearer <jwt>" header on every call: HS256 only,
// constant-time signature check, exp (and nbf) checked. It returns an error message on failure.
func (l *loop) authenticate(h []byte) (uid int64, username, msg string) {
	const invalid = "invalid or expired token"
	if len(h) < 7 || string(h[:7]) != "Bearer " {
		return 0, "", "missing bearer token"
	}
	tok := h[7:]
	l.jwt = l.jwt[:0]
	d1 := bytes.IndexByte(tok, '.')
	d2 := d1 + 1 + bytes.IndexByte(tok[d1+1:], '.')
	if d1 < 0 || d2 <= d1 || bytes.IndexByte(tok[d2+1:], '.') >= 0 {
		return 0, "", invalid
	}
	header, payload, sig := l.decode(tok[:d1]), l.decode(tok[d1+1:d2]), l.decode(tok[d2+1:])
	if header == nil || payload == nil || sig == nil {
		return 0, "", invalid
	}
	var alg string
	var hdr map[string]json.RawMessage
	if json.Unmarshal(header, &hdr) != nil || json.Unmarshal(hdr["alg"], &alg) != nil || alg != "HS256" {
		return 0, "", invalid
	}
	l.mac.Reset()
	l.mac.Write(tok[:d2])
	l.sum = l.mac.Sum(l.sum[:0])
	if !hmac.Equal(l.sum, sig) {
		return 0, "", invalid
	}

	var claims map[string]json.RawMessage
	if json.Unmarshal(payload, &claims) != nil || claims == nil {
		return 0, "", invalid
	}
	now := float64(time.Now().Unix())
	if v, ok := claims["exp"]; ok {
		if exp, isNum := number(v); !isNum || now >= exp {
			return 0, "", invalid
		}
	}
	if v, ok := claims["nbf"]; ok {
		if nbf, isNum := number(v); !isNum || now < nbf {
			return 0, "", invalid
		}
	}
	var sub string
	if v := claims["sub"]; len(v) == 0 || v[0] != '"' || json.Unmarshal(v, &sub) != nil {
		return 0, "", "invalid token payload"
	}
	if uid, _ = parseID(sub); uid == 0 { // not a positive integer, or too big for a user id
		return 0, "", "invalid token payload"
	}
	if v := claims["username"]; len(v) == 0 || v[0] != '"' || json.Unmarshal(v, &username) != nil {
		return 0, "", "invalid token payload"
	}
	return uid, username, ""
}

// decode base64url-decodes one JWT part into the next free region of l.jwt, so the three parts of
// a token don't overwrite each other. Returns nil if the part isn't valid base64url.
func (l *loop) decode(part []byte) []byte {
	n := b64.DecodedLen(len(part))
	if cap(l.jwt)-len(l.jwt) < n {
		grown := make([]byte, len(l.jwt), 2*cap(l.jwt)+n)
		copy(grown, l.jwt)
		l.jwt = grown
	}
	dst := l.jwt[len(l.jwt) : len(l.jwt)+n]
	m, err := b64.Decode(dst, part)
	if err != nil {
		return nil
	}
	l.jwt = l.jwt[:len(l.jwt)+m]
	return dst[:m:m]
}

func number(v json.RawMessage) (float64, bool) {
	if len(v) == 0 || (v[0] != '-' && (v[0] < '0' || v[0] > '9')) {
		return 0, false
	}
	var f float64
	return f, json.Unmarshal(v, &f) == nil
}

// ---- JSON output ----

func appendPost(b []byte, s *stmt) []byte {
	return appendPostFields(b, s.int64(0), s.text(1), s.text(2), s.text(3), s.int64(4))
}

func appendPostFields(b []byte, id int64, body, createdAt, author string, likes int64) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":`...)
	b = appendString(b, body)
	b = append(b, `,"created_at":`...)
	b = appendString(b, createdAt)
	b = append(b, `,"author":`...)
	b = appendString(b, author)
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, likes, 10)
	return append(b, '}')
}

// view is b as a string without copying; b must not change while the string is in use.
func view(b []byte) string { return unsafe.String(unsafe.SliceData(b), len(b)) }

func appendError(b []byte, msg string) []byte {
	b = append(b, `{"error":`...)
	b = appendString(b, msg)
	return append(b, '}')
}

// escape[c] is the escape sequence for byte c if a JSON string must escape it, else empty.
var escape = func() (t [256]string) {
	for c := range 0x20 {
		t[c] = `\u00` + string("0123456789abcdef"[c>>4]) + string("0123456789abcdef"[c&15])
	}
	t['\b'], t['\f'], t['\n'], t['\r'], t['\t'] = `\b`, `\f`, `\n`, `\r`, `\t`
	t['"'], t['\\'] = `\"`, `\\`
	return
}()

func appendString(b []byte, s string) []byte {
	b = append(b, '"')
	start := 0
	for i := range len(s) {
		if e := escape[s[i]]; e != "" {
			b = append(b, s[start:i]...)
			b = append(b, e...)
			start = i + 1
		}
	}
	b = append(b, s[start:]...)
	return append(b, '"')
}
