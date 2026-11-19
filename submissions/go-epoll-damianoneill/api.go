package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"log"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

type app struct {
	db      *db
	started time.Time
	body    []byte // response body scratch space, reused across requests
}

// handle appends the complete HTTP response for r to out.
func (a *app) handle(r *request, out []byte) []byte {
	status, body := a.route(r, a.body[:0])
	a.body = body
	return appendResponse(out, status, body, r.close)
}

func (a *app) route(r *request, b []byte) (int, []byte) {
	get, post := string(r.method) == "GET", string(r.method) == "POST"
	switch path := string(r.path); {
	case path == "/feed" && get:
		return a.feed(b)
	case path == "/health" && get:
		return a.health(b)
	case path == "/posts" && post:
		return a.createPost(r, b)
	}
	if rest, ok := bytes.CutPrefix(r.path, []byte("/posts/")); ok {
		if id, ok := bytes.CutSuffix(rest, []byte("/like")); ok && post {
			return a.like(r, id, b)
		}
		if get && bytes.IndexByte(rest, '/') < 0 {
			return a.getPost(rest, b)
		}
	}
	return fail(b, 404, "not found")
}

func fail(b []byte, status int, message string) (int, []byte) {
	b = append(b[:0], `{"error":"`...)
	b = append(b, message...)
	return status, append(b, `"}`...)
}

func internalError(b []byte, err error) (int, []byte) {
	log.Print(err)
	return fail(b, 500, "internal server error")
}

func (a *app) health(b []byte) (int, []byte) {
	if err := a.db.ping(); err != nil {
		b = append(b, `{"status":"degraded","db":"unreachable","error":`...)
		b = appendJSONString(b, err.Error())
		return 503, append(b, '}')
	}
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started).Seconds()), 10)
	return 200, append(b, '}')
}

func (a *app) feed(b []byte) (int, []byte) {
	rows, err := a.db.feedRows()
	if err != nil {
		return internalError(b, err)
	}
	b = append(b, `{"posts":[`...)
	b, _ = appendPosts(b, rows)
	return 200, append(b, "]}"...)
}

func (a *app) getPost(idText, b []byte) (int, []byte) {
	id, ok := parseID(idText)
	if !ok {
		return fail(b, 400, "invalid post id")
	}
	rows, err := a.db.postRows(id)
	switch {
	case err != nil:
		return internalError(b, err)
	case len(rows) == 0:
		return fail(b, 404, "post not found")
	}
	b = append(b, `{"post":`...)
	b, _ = appendPosts(b, rows)
	return 200, append(b, '}')
}

func (a *app) createPost(r *request, b []byte) (int, []byte) {
	userID, username, msg := authenticate(r.auth, time.Now().Unix())
	if msg != "" {
		return fail(b, 401, msg)
	}
	body, msg := parsePostBody(r.body)
	if msg != "" {
		return fail(b, 400, msg)
	}
	id, created, err := a.db.insertPost(userID, body)
	if err != nil {
		return internalError(b, err)
	}
	b = append(b, `{"post":{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":`...)
	b = appendJSONString(b, body)
	b = append(b, `,"created_at":`...)
	b = appendJSONString(b, created)
	b = append(b, `,"author":`...)
	b = appendJSONString(b, username)
	return 201, append(b, `,"like_count":0}}`...)
}

func (a *app) like(r *request, idText, b []byte) (int, []byte) {
	userID, _, msg := authenticate(r.auth, time.Now().Unix())
	if msg != "" {
		return fail(b, 401, msg)
	}
	id, ok := parseID(idText)
	if !ok {
		return fail(b, 400, "invalid post id")
	}
	inserted, exists, err := a.db.likePost(userID, id)
	switch {
	case err != nil:
		return internalError(b, err)
	case !exists:
		return fail(b, 404, "post not found")
	}
	status := 200
	b = append(b, `{"liked":true,"already_liked":`...)
	if inserted {
		status = 201
		b = append(b, "false"...)
	} else {
		b = append(b, "true"...)
	}
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	return status, append(b, '}')
}

// parseID accepts a positive integer in plain decimal digits.
func parseID(s []byte) (int64, bool) {
	if len(s) == 0 {
		return 0, false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	id, err := strconv.ParseInt(string(s), 10, 64)
	return id, err == nil && id > 0
}

// parsePostBody returns the trimmed post body, or the spec's 400 message.
func parsePostBody(b []byte) (string, string) {
	if !json.Valid(b) {
		return "", "malformed JSON body"
	}
	var v map[string]any
	_ = json.Unmarshal(b, &v) // valid JSON that is not an object leaves v nil
	body, _ := v["body"].(string)
	body = strings.TrimFunc(body, isJSSpace)
	if body == "" {
		return "", "body is required"
	}
	if utf8.RuneCountInString(body) > 500 {
		return "", "body must be at most 500 characters"
	}
	return body, ""
}

// isJSSpace is the whitespace set of JavaScript's String.prototype.trim.
func isJSSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

const hexDigits = "0123456789abcdef"

// appendPosts appends a post object, comma separated, for each row packed by step_rows from
// postSelect, and returns how many it appended.
func appendPosts(out, rows []byte) ([]byte, int) {
	n := 0
	for len(rows) > 0 {
		var id, body, created, author, likes []byte
		id, rows = nextColumn(rows)
		body, rows = nextColumn(rows)
		created, rows = nextColumn(rows)
		author, rows = nextColumn(rows)
		likes, rows = nextColumn(rows)
		if n > 0 {
			out = append(out, ',')
		}
		out = append(out, `{"id":`...)
		out = append(out, id...)
		out = append(out, `,"body":`...)
		out = appendJSONString(out, body)
		out = append(out, `,"created_at":`...)
		out = appendJSONString(out, created)
		out = append(out, `,"author":`...)
		out = appendJSONString(out, author)
		out = append(out, `,"like_count":`...)
		out = append(out, likes...)
		out = append(out, '}')
		n++
	}
	return out, n
}

func nextColumn(rows []byte) (column, rest []byte) {
	n := binary.NativeEndian.Uint32(rows)
	return rows[4 : 4+n], rows[4+n:]
}

func appendJSONString[T string | []byte](out []byte, s T) []byte {
	out = append(out, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		out = append(out, s[start:i]...)
		switch c {
		case '"', '\\':
			out = append(out, '\\', c)
		case '\n':
			out = append(out, '\\', 'n')
		case '\r':
			out = append(out, '\\', 'r')
		case '\t':
			out = append(out, '\\', 't')
		default:
			out = append(out, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		start = i + 1
	}
	out = append(out, s[start:]...)
	return append(out, '"')
}
