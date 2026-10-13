package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"hash"
	"math"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
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
)

var started = time.Now()

// api is the request state: the SQLite connection with its prepared statements, and the JWT key.
// Nothing in here remembers data between requests (rule 5).
type api struct {
	db                                         *db
	health, feed, post, insertPost, insertLike *stmt
	postExists                                 *stmt
	mac                                        hash.Hash
	sum                                        []byte
	jwt                                        []byte // decoded JWT parts
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
		"PRAGMA busy_timeout = 5000",    // in case another process has the file open at startup
		"PRAGMA mmap_size = 1073741824", // read pages straight from the OS page cache
		"PRAGMA cache_size = -16384",    // 16 MiB
		"PRAGMA temp_store = MEMORY",
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

func (l *loop) getFeed(r *request) {
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
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, s.int64(0), 10)
	b = append(b, `,"body":`...)
	b = appendString(b, s.text(1))
	b = append(b, `,"created_at":`...)
	b = appendString(b, s.text(2))
	b = append(b, `,"author":`...)
	b = appendString(b, s.text(3))
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, s.int64(4), 10)
	return append(b, '}')
}

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
