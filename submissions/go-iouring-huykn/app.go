package main

import (
	"bytes"
	"time"
	"unicode/utf8"
)

// app routes requests and appends complete HTTP responses to an output buffer.
type app struct {
	jwt     *verifier
	started time.Time
	body    []byte
	now     int64 // unix seconds, refreshed every pass
}

func newApp(secret []byte) *app {
	return &app{jwt: newVerifier(secret), started: time.Now(), body: make([]byte, 0, 4096)}
}

// start writes the status line and headers. Content-Length is a 5-character field, right-aligned
// with leading spaces (optional whitespace before a field value, RFC 9110 5.6.3), filled in by end.
func start(b []byte, status int) ([]byte, int) {
	switch status {
	case 200:
		b = append(b, "HTTP/1.1 200 OK\r\n"...)
	case 201:
		b = append(b, "HTTP/1.1 201 Created\r\n"...)
	case 400:
		b = append(b, "HTTP/1.1 400 Bad Request\r\n"...)
	case 401:
		b = append(b, "HTTP/1.1 401 Unauthorized\r\n"...)
	case 404:
		b = append(b, "HTTP/1.1 404 Not Found\r\n"...)
	case 503:
		b = append(b, "HTTP/1.1 503 Service Unavailable\r\n"...)
	default:
		b = append(b, "HTTP/1.1 500 Internal Server Error\r\n"...)
	}
	b = append(b, "Content-Type: application/json\r\nContent-Length:     0\r\n\r\n"...)
	return b, len(b)
}

// end fills in Content-Length for the body that starts at body.
func end(b []byte, body int) []byte {
	n := len(b) - body
	f := b[body-9 : body-4] // the 5 characters before "\r\n\r\n"
	if n > 99999 {
		// Longer than the field: move the body to make room (bodies are far smaller than this).
		var d [20]byte
		ds := appendInt(d[:0], int64(n))
		extra := len(ds) - 5
		b = append(b, make([]byte, extra)...)
		copy(b[body+extra:], b[body:body+n])
		copy(b[body-9:], ds)
		copy(b[body-9+len(ds):], "\r\n\r\n")
		return b
	}
	for i := 4; i >= 0; i-- {
		if n == 0 && i < 4 {
			f[i] = ' '
			continue
		}
		f[i] = byte('0' + n%10)
		n /= 10
	}
	return b
}

func errorResp(b []byte, status int, msg string) []byte {
	b, s := start(b, status)
	b = append(b, `{"error":"`...)
	b = append(b, msg...)
	b = append(b, `"}`...)
	return end(b, s)
}

// handle answers one request; dispatch and path validation are separate.
func (a *app) handle(r *request, b []byte) []byte {
	switch string(r.method) {
	case "GET":
		return a.handleGet(r.path, b)
	case "POST":
		return a.handlePost(r, b)
	default:
		return errorResp(b, 404, "not found")
	}
}

func (a *app) handleGet(path, b []byte) []byte {
	if string(path) == "/feed" {
		return a.feed(b)
	}
	if len(path) > 7 && string(path[:7]) == "/posts/" {
		return a.getPostID(path[7:], b)
	}
	if string(path) == "/health" {
		return a.health(b)
	}
	return errorResp(b, 404, "not found")
}

func (a *app) getPostID(rawID, b []byte) []byte {
	id, ok := positive(rawID)
	if ok {
		return a.post(b, id)
	}
	if bytes.IndexByte(rawID, '/') >= 0 {
		return errorResp(b, 404, "not found")
	}
	return errorResp(b, 400, "invalid post id")
}

func (a *app) handlePost(r *request, b []byte) []byte {
	path := r.path
	if string(path) == "/posts" {
		return a.create(r, b)
	}
	if len(path) <= 12 || string(path[:7]) != "/posts/" || string(path[len(path)-5:]) != "/like" {
		return errorResp(b, 404, "not found")
	}
	id := path[7 : len(path)-5]
	if bytes.IndexByte(id, '/') >= 0 {
		return errorResp(b, 404, "not found")
	}
	return a.like(r, id, b)
}

// appendPost serializes directly into the connection's output buffer. All
// fields are rebuilt for this request; no intermediate object is retained.
func (a *app) appendPost(dst []byte, id int64, body, created, author []byte, likes int64) []byte {
	dst = append(dst, `{"id":`...)
	dst = appendInt(dst, id)
	dst = append(dst, `,"body":"`...)
	dst = appendEsc(dst, body)
	dst = append(dst, `","created_at":"`...)
	dst = appendEsc(dst, created)
	dst = append(dst, `","author":"`...)
	dst = appendEsc(dst, author)
	dst = append(dst, `","like_count":`...)
	dst = appendInt(dst, likes)
	return append(dst, '}')
}

func (a *app) feed(b []byte) []byte {
	rows, ok := dbFeed()
	if !ok {
		return errorResp(b, 500, "internal server error")
	}
	b, s := start(b, 200)
	b = append(b, `{"posts":[`...)
	for i, n := 0, rows.len(); i < n; i++ {
		r := rows.at(i)
		if i > 0 {
			b = append(b, ',')
		}
		b = a.appendFeedPost(b, int64(r.key), rows.text(r.content), rows.text(r.timestamp), rows.text(r.username), int64(r.count))
	}
	b = append(b, "]}"...)
	return end(b, s)
}

func (a *app) post(b []byte, id int64) []byte {
	record, status := dbPost(id)
	switch status {
	case 1:
	case 0:
		return errorResp(b, 404, "post not found")
	default:
		return errorResp(b, 500, "internal server error")
	}
	b, s := start(b, 200)
	b = append(b, `{"post":`...)
	b = a.appendPost(b, record.id, record.body, record.created, record.author, record.likes)
	b = append(b, '}')
	return end(b, s)
}

func (a *app) health(b []byte) []byte {
	ok, msg := dbHealth()
	if !ok {
		b, s := start(b, 503)
		b = append(b, `{"status":"degraded","db":"unreachable","error":"`...)
		b = appendEsc(b, msg)
		b = append(b, `"}`...)
		return end(b, s)
	}
	b, s := start(b, 200)
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = appendInt(b, int64(time.Since(a.started)/time.Second))
	b = append(b, '}')
	return end(b, s)
}

// auth returns the user, or ok=false with the 401 already written.
func (a *app) auth(r *request, b []byte) ([]byte, int64, []byte, bool) {
	st, uid, name := a.jwt.verify(r.auth, r.hasAuth, a.now)
	switch st {
	case authOK:
		return b, uid, name, true
	case authMissing:
		return errorResp(b, 401, "missing bearer token"), 0, nil, false
	case authInvalid:
		return errorResp(b, 401, "invalid or expired token"), 0, nil, false
	default:
		return errorResp(b, 401, "invalid token payload"), 0, nil, false
	}
}

func (a *app) like(r *request, rawID []byte, b []byte) []byte {
	b, uid, _, ok := a.auth(r, b)
	if !ok {
		return b
	}
	id, ok := positive(rawID)
	if !ok {
		return errorResp(b, 400, "invalid post id")
	}
	res := dbLike(uid, id)
	var s int
	switch res {
	case 2:
		b, s = start(b, 201)
		b = append(b, `{"liked":true,"already_liked":false,"post_id":`...)
	case 1:
		b, s = start(b, 200)
		b = append(b, `{"liked":true,"already_liked":true,"post_id":`...)
	case 0:
		return errorResp(b, 404, "post not found")
	default:
		return errorResp(b, 500, "internal server error")
	}
	b = appendInt(b, id)
	b = append(b, '}')
	return end(b, s)
}

// jsSpace reports JavaScript's String.prototype.trim whitespace.
func jsSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

func trimJS(s []byte) []byte {
	for len(s) > 0 {
		if s[0] < 0x80 {
			if !jsSpace(rune(s[0])) {
				break
			}
			s = s[1:]
			continue
		}
		r, n := utf8.DecodeRune(s)
		if !jsSpace(r) {
			break
		}
		s = s[n:]
	}
	for len(s) > 0 {
		c := s[len(s)-1]
		if c < 0x80 {
			if !jsSpace(rune(c)) {
				break
			}
			s = s[:len(s)-1]
			continue
		}
		r, n := utf8.DecodeLastRune(s)
		if !jsSpace(r) {
			break
		}
		s = s[:len(s)-n]
	}
	return s
}

func (a *app) create(r *request, b []byte) []byte {
	b, uid, name, ok := a.auth(r, b)
	if !ok {
		return b
	}
	var body byteValue
	valid, isObj := walkObject(r.body, a.body[:0], func(key []byte, value byteValue) {
		if string(key) == "body" {
			body = value
		}
	})
	if !valid {
		return errorResp(b, 400, "malformed JSON body")
	}
	if !isObj || body.kind != byteString {
		return errorResp(b, 400, "body is required")
	}
	txt := body.raw
	if body.escaped {
		a.body = unescape(body.raw, a.body[:0])
		txt = a.body
	}
	txt = trimJS(txt)
	if len(txt) == 0 {
		return errorResp(b, 400, "body is required")
	}
	if len(txt) > 500 && utf8.RuneCount(txt) > 500 {
		return errorResp(b, 400, "body must be at most 500 characters")
	}
	id, created, ok := dbInsertPost(uid, txt)
	if !ok {
		return errorResp(b, 500, "internal server error")
	}
	b, s := start(b, 201)
	b = append(b, `{"post":`...)
	b = a.appendPost(b, id, txt, created, name, 0)
	b = append(b, '}')
	return end(b, s)
}
