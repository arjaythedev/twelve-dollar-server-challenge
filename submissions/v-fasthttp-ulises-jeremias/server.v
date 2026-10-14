module main

import fasthttp
import json2
import crypto.hmac
import crypto.sha256
import encoding.base64
import os
import time

#pkgconfig sqlite3
#include <sqlite3.h>

struct C.sqlite3 {}

struct C.sqlite3_stmt {}

fn C.sqlite3_open_v2(filename &char, db &&C.sqlite3, flags i32, vfs &char) i32
fn C.sqlite3_busy_timeout(db &C.sqlite3, ms i32) i32
fn C.sqlite3_exec(db &C.sqlite3, query &char, callback voidptr, arg voidptr, error_msg &&char) i32
fn C.sqlite3_prepare_v2(db &C.sqlite3, query &char, bytes i32, stmt &&C.sqlite3_stmt, tail voidptr) i32
fn C.sqlite3_step(stmt &C.sqlite3_stmt) i32
fn C.sqlite3_reset(stmt &C.sqlite3_stmt) i32
fn C.sqlite3_clear_bindings(stmt &C.sqlite3_stmt) i32
fn C.sqlite3_bind_int64(stmt &C.sqlite3_stmt, index i32, value i64) i32
fn C.sqlite3_bind_text(stmt &C.sqlite3_stmt, index i32, value &char, bytes i32, destructor voidptr) i32
fn C.sqlite3_column_int64(stmt &C.sqlite3_stmt, index i32) i64
fn C.sqlite3_column_text(stmt &C.sqlite3_stmt, index i32) &u8
fn C.sqlite3_column_bytes(stmt &C.sqlite3_stmt, index i32) i32
fn C.sqlite3_changes(db &C.sqlite3) i32

const sqlite_ok = 0
const sqlite_row = 100
const sqlite_done = 101
const sqlite_open_readwrite = 0x00000002
const sqlite_open_create = 0x00000004
const sqlite_open_nomutex = 0x00008000
const sqlite_transient = unsafe { voidptr(-1) }

struct State {
mut:
	db          &C.sqlite3      = unsafe { nil }
	feed_stmt   &C.sqlite3_stmt = unsafe { nil }
	post_stmt   &C.sqlite3_stmt = unsafe { nil }
	health_stmt &C.sqlite3_stmt = unsafe { nil }
	insert_post &C.sqlite3_stmt = unsafe { nil }
	insert_like &C.sqlite3_stmt = unsafe { nil }
	exists_stmt &C.sqlite3_stmt = unsafe { nil }
	secret      string
	started     i64
	mac         &hmac.Hmac[&sha256.Digest] = unsafe { nil }
}

struct Post {
	id         int
	body       string
	created_at string
	author     string
	like_count int
}

struct PostEnvelope {
	post Post
}

struct FeedEnvelope {
	posts []Post
}

struct ErrorEnvelope {
	error string
}

struct LikeEnvelope {
	liked         bool
	already_liked bool
	post_id       int
}

struct HealthEnvelope {
	status   string
	db       string
	uptime_s i64
}

struct DegradedHealthEnvelope {
	status string
	db     string
	error  string
}

fn make_state() voidptr {
	path := os.getenv('SQLITE_PATH')
	mut db := &C.sqlite3(unsafe { nil })
	rc := C.sqlite3_open_v2(&char(path.str), &db, sqlite_open_readwrite | sqlite_open_create | sqlite_open_nomutex, unsafe { nil })
	if rc != sqlite_ok { panic('sqlite3_open_v2 failed (${rc})') }
	C.sqlite3_busy_timeout(db, 5000)
	exec_sql(db, 'PRAGMA journal_mode=WAL')
	exec_sql(db, 'PRAGMA synchronous=NORMAL')
	exec_sql(db, 'PRAGMA mmap_size=1073741824')
	exec_sql(db, 'PRAGMA cache_size=-65536')
	secret := os.getenv('JWT_SECRET')
	return &State{
		db:          db
		feed_stmt:   prepare(db, 'SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20')
		post_stmt:   prepare(db, 'SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1')
		health_stmt: prepare(db, 'SELECT 1')
		insert_post: prepare(db, 'INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at')
		insert_like: prepare(db, 'INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING')
		exists_stmt: prepare(db, 'SELECT 1 FROM posts WHERE id=?1')
		secret:      secret
		started:     time.now().unix()
		mac:         hmac.new_hmac(sha256.new, secret.bytes())
	}
}

fn exec_sql(db &C.sqlite3, query string) {
	if C.sqlite3_exec(db, &char(query.str), unsafe { nil }, unsafe { nil }, unsafe { nil }) != sqlite_ok {
		panic('SQLite pragma failed')
	}
}

fn prepare(db &C.sqlite3, query string) &C.sqlite3_stmt {
	mut stmt := &C.sqlite3_stmt(unsafe { nil })
	if C.sqlite3_prepare_v2(db, &char(query.str), -1, &stmt, unsafe { nil }) != sqlite_ok {
		panic('SQLite statement preparation failed')
	}
	return stmt
}

fn reset(stmt &C.sqlite3_stmt) {
	C.sqlite3_reset(stmt)
	C.sqlite3_clear_bindings(stmt)
}

fn column_text(stmt &C.sqlite3_stmt, index int) string {
	ptr := C.sqlite3_column_text(stmt, index)
	if ptr == unsafe { nil } { return '' }
	return unsafe { tos(ptr, C.sqlite3_column_bytes(stmt, index)) }.clone()
}

fn column_post(stmt &C.sqlite3_stmt) Post {
	return Post{
		id:         int(C.sqlite3_column_int64(stmt, 0))
		body:       column_text(stmt, 1)
		created_at: column_text(stmt, 2)
		author:     column_text(stmt, 3)
		like_count: int(C.sqlite3_column_int64(stmt, 4))
	}
}

@[direct_array_access]
fn bytes(req fasthttp.HttpRequest, slice fasthttp.Slice) []u8 {
	if slice.len <= 0 { return []u8{} }
	return req.buffer[slice.start..slice.start + slice.len]
}

fn append_response(mut out []u8, status int, reason string, body string, close bool) {
	out << 'HTTP/1.1 ${status} ${reason}\r\nContent-Type: application/json\r\nContent-Length: ${body.len}\r\nConnection: ${if close {
		'close'
	} else {
		'keep-alive'
	}}\r\n\r\n'.bytes()
	out << body.bytes()
}

fn error_response(mut out []u8, status int, message string, mut ctl fasthttp.ResponseControl) fasthttp.Step {
	append_response(mut out, status, status_reason(status), json2.encode(ErrorEnvelope{message}, json2.EncoderOptions{}), ctl.should_close)
	return .done
}

fn status_reason(status int) string {
	return match status {
		200 { 'OK' }
		201 { 'Created' }
		400 { 'Bad Request' }
		401 { 'Unauthorized' }
		404 { 'Not Found' }
		503 { 'Service Unavailable' }
		else { 'Internal Server Error' }
	}
}

fn header_value(req fasthttp.HttpRequest, name string) string {
	h := bytes(req, req.header_fields)
	mut i := 0
	for i < h.len {
		start := i
		for i < h.len && h[i] != `\r` && h[i] != `\n` { i++ }
		end := i
		for i < h.len && (h[i] == `\r` || h[i] == `\n`) { i++ }
		mut colon := start
		for colon < end && h[colon] != `:` { colon++ }
		if colon - start == name.len {
			mut same := true
			for j in 0 .. name.len {
				c := h[start + j]
				lower := if c >= `A` && c <= `Z` { c + 32 } else { c }
				if lower != name[j] {
					same = false
					break
				}
			}
			if same {
				mut value := colon + 1
				for value < end && (h[value] == ` ` || h[value] == `\t`) { value++ }
				return h[value..end].bytestr()
			}
		}
	}
	return ''
}

fn positive_id(s string) int {
	if s.len == 0 { return 0 }
	mut n := i64(0)
	for c in s {
		if c < `0` || c > `9` { return 0 }
		digit := i64(c - `0`)
		if n > (9007199254740991 - digit) / 10 { return 0 }
		n = n * 10 + digit
	}
	return int(n)
}

@[direct_array_access]
fn positive_id_bytes(s []u8) int {
	if s.len == 0 { return 0 }
	mut n := i64(0)
	for c in s {
		if c < `0` || c > `9` { return 0 }
		digit := i64(c - `0`)
		if n > (9007199254740991 - digit) / 10 { return 0 }
		n = n * 10 + digit
	}
	return int(n)
}

fn is_numeric(value json2.Any) bool {
	return match value {
		int, i64, f64 { true }
		else { false }
	}
}

fn authenticate(req fasthttp.HttpRequest, mut st State) (int, string, string) {
	auth := header_value(req, 'authorization')
	if auth == '' || !auth.starts_with('Bearer ') { return 2, '', '' }
	token := auth[7..]
	parts := token.split('.')
	if parts.len != 3 { return 401, '', '' }
	header := json2.decode[map[string]json2.Any](base64.url_decode_str(parts[0]), json2.DecoderOptions{}) or { return 401, '', '' }
	alg := header['alg'] or { return 401, '', '' }
	if alg !is string || alg.str() != 'HS256' { return 401, '', '' }
	mut expected := []u8{len: 32}
	st.mac.write('${parts[0]}.${parts[1]}'.bytes()) or { return 401, '', '' }
	st.mac.sum_into(mut expected)
	actual := base64.url_decode(parts[2])
	if !hmac.equal(expected, actual) { return 401, '', '' }
	claims := json2.decode[map[string]json2.Any](base64.url_decode_str(parts[1]), json2.DecoderOptions{}) or { return 401, '', '' }
	exp := claims['exp'] or { return 401, '', '' }
	if !is_numeric(exp) { return 401, '', '' }
	if exp.i64() <= time.now().unix() { return 401, '', '' }
	if nbf := claims['nbf'] {
		if !is_numeric(nbf) { return 401, '', '' }
		if nbf.i64() > time.now().unix() { return 401, '', '' }
	}
	sub := claims['sub'] or { return 403, '', '' }
	username := claims['username'] or { return 403, '', '' }
	if sub !is string || username !is string {
		return 403, '', ''
	}
	user_id := sub.str()
	if positive_id(user_id) == 0 { return 403, '', '' }
	return 0, user_id, username.str()
}

fn handle(req fasthttp.HttpRequest, mut out []u8, worker_state voidptr, mut ctl fasthttp.ResponseControl) fasthttp.Step {
	mut st := unsafe { &State(worker_state) }
	// Keep descriptor headroom for SQLite, logs, and transient accepts. The
	// challenge box uses a 65,535 descriptor limit; clients reconnect on their
	// next request after the response advertises Connection: close.
	should_close := req.client_conn_fd >= 60000
	ctl.should_close = should_close
	path := bytes(req, req.path)
	method := bytes(req, req.method)
	if method == 'GET'.bytes() && path == '/health'.bytes() {
		rc := C.sqlite3_step(st.health_stmt)
		reset(st.health_stmt)
		if rc != sqlite_row {
			body := json2.encode(DegradedHealthEnvelope{'degraded', 'unreachable', 'database query failed'}, json2.EncoderOptions{})
			append_response(mut out, 503, 'Service Unavailable', body, should_close)
			return .done
		}
		body := json2.encode(HealthEnvelope{'ok', 'ok', time.now().unix() - st.started}, json2.EncoderOptions{})
		append_response(mut out, 200, 'OK', body, should_close)
		return .done
	}
	if method == 'GET'.bytes() && path == '/feed'.bytes() {
		mut posts := []Post{cap: 20}
		for {
			rc := C.sqlite3_step(st.feed_stmt)
			if rc == sqlite_done { break }
			if rc != sqlite_row {
				reset(st.feed_stmt)
				return error_response(mut out, 500, 'internal server error', mut ctl)
			}
			posts << column_post(st.feed_stmt)
		}
		reset(st.feed_stmt)
		append_response(mut out, 200, 'OK', json2.encode(FeedEnvelope{posts}, json2.EncoderOptions{}), should_close)
		return .done
	}
	if method == 'GET'.bytes() && path.len > 7 && path[..7] == '/posts/'.bytes() {
		id := positive_id_bytes(path[7..])
		if id == 0 { return error_response(mut out, 400, 'invalid post id', mut ctl) }
		C.sqlite3_bind_int64(st.post_stmt, 1, i64(id))
		rc := C.sqlite3_step(st.post_stmt)
		if rc != sqlite_row {
			reset(st.post_stmt)
			if rc == sqlite_done { return error_response(mut out, 404, 'post not found', mut ctl) }
			return error_response(mut out, 500, 'internal server error', mut ctl)
		}
		post := column_post(st.post_stmt)
		reset(st.post_stmt)
		append_response(mut out, 200, 'OK', json2.encode(PostEnvelope{post}, json2.EncoderOptions{}), should_close)
		return .done
	}
	if method == 'POST'.bytes() && path == '/posts'.bytes() {
		code, user_id, username := authenticate(req, mut st)
		if code == 2 { return error_response(mut out, 401, 'missing bearer token', mut ctl) }
		if code == 401 { return error_response(mut out, 401, 'invalid or expired token', mut ctl) }
		if code != 0 { return error_response(mut out, 401, 'invalid token payload', mut ctl) }
		payload := json2.decode[map[string]json2.Any](bytes(req, req.body).bytestr(), json2.DecoderOptions{}) or { return error_response(mut out, 400, 'malformed JSON body', mut ctl) }
		body_value := payload['body'] or { return error_response(mut out, 400, 'body is required', mut ctl) }
		if body_value !is string {
			return error_response(mut out, 400, 'body is required', mut ctl)
		}
		body_text := body_value.str().trim_space()
		if body_text.len == 0 { return error_response(mut out, 400, 'body is required', mut ctl) }
		if body_text.runes().len > 500 {
			return error_response(mut out, 400, 'body must be at most 500 characters', mut ctl)
		}
		C.sqlite3_bind_int64(st.insert_post, 1, i64(positive_id(user_id)))
		C.sqlite3_bind_text(st.insert_post, 2, &char(body_text.str), body_text.len, sqlite_transient)
		rc := C.sqlite3_step(st.insert_post)
		if rc != sqlite_row {
			reset(st.insert_post)
			return error_response(mut out, 500, 'internal server error', mut ctl)
		}
		id := int(C.sqlite3_column_int64(st.insert_post, 0))
		created_at := column_text(st.insert_post, 1)
		if C.sqlite3_step(st.insert_post) != sqlite_done {
			reset(st.insert_post)
			return error_response(mut out, 500, 'internal server error', mut ctl)
		}
		reset(st.insert_post)
		p := Post{id, body_text, created_at, username, 0}
		append_response(mut out, 201, 'Created', json2.encode(PostEnvelope{p}, json2.EncoderOptions{}), should_close)
		return .done
	}
	if method == 'POST'.bytes() && path.len > 11 && path[path.len - 5..] == '/like'.bytes() {
		code, user_id, _ := authenticate(req, mut st)
		if code == 2 { return error_response(mut out, 401, 'missing bearer token', mut ctl) }
		if code == 401 { return error_response(mut out, 401, 'invalid or expired token', mut ctl) }
		if code != 0 { return error_response(mut out, 401, 'invalid token payload', mut ctl) }
		id := positive_id_bytes(path[7..path.len - 5])
		if id == 0 { return error_response(mut out, 400, 'invalid post id', mut ctl) }
		C.sqlite3_bind_int64(st.insert_like, 1, i64(positive_id(user_id)))
		C.sqlite3_bind_int64(st.insert_like, 2, i64(id))
		if C.sqlite3_step(st.insert_like) != sqlite_done {
			reset(st.insert_like)
			return error_response(mut out, 500, 'internal server error', mut ctl)
		}
		changed := C.sqlite3_changes(st.db)
		reset(st.insert_like)
		if changed > 0 {
			append_response(mut out, 201, 'Created', json2.encode(LikeEnvelope{true, false, id}, json2.EncoderOptions{}), should_close)
			return .done
		}
		C.sqlite3_bind_int64(st.exists_stmt, 1, i64(id))
		exists := C.sqlite3_step(st.exists_stmt) == sqlite_row
		reset(st.exists_stmt)
		if !exists { return error_response(mut out, 404, 'post not found', mut ctl) }
		append_response(mut out, 200, 'OK', json2.encode(LikeEnvelope{true, true, id}, json2.EncoderOptions{}), should_close)
		return .done
	}
	return error_response(mut out, 404, 'not found', mut ctl)
}

fn main() {
	port := os.getenv('PORT').int()
	mut server := fasthttp.new_server(fasthttp.ServerConfig{
		host:               if os.getenv('HOST') == '' { '127.0.0.1' } else { os.getenv('HOST') }
		port:               if port > 0 { port } else { 3000 }
		append_handler:     handle
		make_state:         make_state
		timeout_in_seconds: 75
	}) or { panic(err) }
	server.run() or { panic(err) }
}
