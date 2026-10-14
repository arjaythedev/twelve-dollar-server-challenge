module main

import crypto.hmac
import crypto.sha256
import db.sqlite
import encoding.base64
import json2
import net.http
import os
import sync
import time

struct App {
mut:
	db      sqlite.DB
	db_lock sync.Mutex
	secret  string
	started i64
}

struct Post {
	id         int
	body       string
	created_at string
	author     string
	like_count int
}

struct PostResponse {
	post Post
}

struct FeedResponse {
	posts []Post
}

struct ErrorResponse {
	error string
}

struct LikeResponse {
	liked         bool
	already_liked bool
	post_id       int
}

struct HealthResponse {
	status   string
	db       string
	uptime_s i64
}

struct DegradedHealthResponse {
	status string
	db     string
	error  string
}

fn (mut app App) handle(req http.Request) http.Response {
	app.db_lock.lock()
	defer {
		app.db_lock.unlock()
	}
	path := req.url.all_before('?')
	if req.method == .get && path == '/health' {
		return app.health()
	}
	if req.method == .get && path == '/feed' {
		return app.feed()
	}
	if req.method == .get && path.starts_with('/posts/') {
		return app.get_post(path['/posts/'.len..])
	}
	if req.method == .post && path == '/posts' {
		return app.create_post(req)
	}
	if req.method == .post && path.starts_with('/posts/') && path.ends_with('/like') {
		return app.like_post(req, path['/posts/'.len..path.len - '/like'.len])
	}
	return json_response(404, ErrorResponse{'not found'})
}

fn (mut app App) health() http.Response {
	app.db.q_int('SELECT 1') or {
		return json_response(503, DegradedHealthResponse{'degraded', 'unreachable', 'database query failed'})
	}
	return json_response(200, HealthResponse{'ok', 'ok', time.now().unix() - app.started})
}

fn (mut app App) feed() http.Response {
	rows := app.db.exec('SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20') or {
		return internal_error()
	}
	mut posts := []Post{cap: rows.len}
	for row in rows {
		posts << post_from_row(row)
	}
	return json_response(200, FeedResponse{posts})
}

fn (mut app App) get_post(raw_id string) http.Response {
	id := parse_positive_id(raw_id)
	if id == 0 {
		return json_response(400, ErrorResponse{'invalid post id'})
	}
	row := app.db.exec_param('SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?', id.str()) or {
		return internal_error()
	}
	if row.len == 0 {
		return json_response(404, ErrorResponse{'post not found'})
	}
	return json_response(200, PostResponse{post_from_row(row[0])})
}

fn (mut app App) create_post(req http.Request) http.Response {
	user_id, username, auth_error := app.authenticate(req)
	if auth_error != '' {
		return json_response(401, ErrorResponse{auth_error})
	}
	payload := json2.decode[map[string]json2.Any](req.data, json2.DecoderOptions{}) or {
		return json_response(400, ErrorResponse{'malformed JSON body'})
	}
	body_value := payload['body'] or { return json_response(400, ErrorResponse{'body is required'}) }
	if body_value !is string {
		return json_response(400, ErrorResponse{'body is required'})
	}
	body := body_value.str().trim_space()
	if body.len == 0 {
		return json_response(400, ErrorResponse{'body is required'})
	}
	if body.runes().len > 500 {
		return json_response(400, ErrorResponse{'body must be at most 500 characters'})
	}
	rows := app.db.exec_param2('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at', user_id.str(), body) or {
		return internal_error()
	}
	if rows.len == 0 {
		return internal_error()
	}
	post := Post{rows[0].get_int('id'), body, rows[0].get_string('created_at'), username, 0}
	return json_response(201, PostResponse{post})
}

fn (mut app App) like_post(req http.Request, raw_id string) http.Response {
	user_id, _, auth_error := app.authenticate(req)
	if auth_error != '' {
		return json_response(401, ErrorResponse{auth_error})
	}
	id := parse_positive_id(raw_id)
	if id == 0 {
		return json_response(400, ErrorResponse{'invalid post id'})
	}
	exists := app.db.q_int('SELECT EXISTS(SELECT 1 FROM posts WHERE id = ${id})') or {
		return internal_error()
	}
	if exists == 0 {
		return json_response(404, ErrorResponse{'post not found'})
	}
	app.db.exec_param2('INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING', user_id.str(), id.str()) or {
		return internal_error()
	}
	if app.db.get_affected_rows_count() > 0 {
		return json_response(201, LikeResponse{true, false, id})
	}
	return json_response(200, LikeResponse{true, true, id})
}

fn (mut app App) authenticate(req http.Request) (int, string, string) {
	authorization := req.header.get(.authorization) or { '' }
	if !authorization.starts_with('Bearer ') {
		return 0, '', 'missing bearer token'
	}
	parts := authorization['Bearer '.len..].split('.')
	if parts.len != 3 || !valid_base64url(parts[0]) || !valid_base64url(parts[1]) || !valid_base64url(parts[2]) {
		return 0, '', 'invalid or expired token'
	}
	header := json2.decode[map[string]json2.Any](base64.url_decode_str(parts[0]), json2.DecoderOptions{}) or {
		return 0, '', 'invalid or expired token'
	}
	algorithm := header['alg'] or { return 0, '', 'invalid or expired token' }
	if algorithm !is string || algorithm.str() != 'HS256' {
		return 0, '', 'invalid or expired token'
	}
	expected := hmac.new(app.secret.bytes(), '${parts[0]}.${parts[1]}'.bytes(), sha256.sum, sha256.block_size)
	actual := base64.url_decode(parts[2])
	if !hmac.equal(expected, actual) {
		return 0, '', 'invalid or expired token'
	}
	claims := json2.decode[map[string]json2.Any](base64.url_decode_str(parts[1]), json2.DecoderOptions{}) or {
		return 0, '', 'invalid or expired token'
	}
	expiration := claims['exp'] or { return 0, '', 'invalid or expired token' }
	if !is_number(expiration) || expiration.i64() <= time.now().unix() {
		return 0, '', 'invalid or expired token'
	}
	subject := claims['sub'] or { return 0, '', 'invalid token payload' }
	username := claims['username'] or { return 0, '', 'invalid token payload' }
	if subject !is string || username !is string {
		return 0, '', 'invalid token payload'
	}
	user_id := parse_positive_id(subject.str())
	if user_id == 0 {
		return 0, '', 'invalid token payload'
	}
	return user_id, username.str(), ''
}

fn valid_base64url(value string) bool {
	if value.len == 0 {
		return false
	}
	for byte in value {
		if !((byte >= `a` && byte <= `z`) || (byte >= `A` && byte <= `Z`) || (byte >= `0` && byte <= `9`) || byte == `-` || byte == `_`) {
			return false
		}
	}
	return true
}

fn is_number(value json2.Any) bool {
	return match value {
		int, i64, f64 { true }
		else { false }
	}
}

fn post_from_row(row sqlite.Row) Post {
	return Post{
		id:         row.get_int('id')
		body:       row.get_string('body')
		created_at: row.get_string('created_at')
		author:     row.get_string('username')
		like_count: row.get_int('like_count')
	}
}

fn parse_positive_id(value string) int {
	if value.len == 0 {
		return 0
	}
	mut id := i64(0)
	for byte in value {
		if byte < `0` || byte > `9` {
			return 0
		}
		digit := i64(byte - `0`)
		if id > (9007199254740991 - digit) / 10 {
			return 0
		}
		id = id * 10 + digit
	}
	return int(id)
}

fn json_response[T](status int, value T) http.Response {
	mut response := http.Response{
		status_code: status
		status_msg:  match status {
			200 { 'OK' }
			201 { 'Created' }
			400 { 'Bad Request' }
			401 { 'Unauthorized' }
			404 { 'Not Found' }
			503 { 'Service Unavailable' }
			else { 'Internal Server Error' }
		}
		http_version: '1.1'
		body:         json2.encode(value, json2.EncoderOptions{})
		header:        http.new_header(key: .content_type, value: 'application/json')
	}
	return response
}

fn internal_error() http.Response {
	return json_response(500, ErrorResponse{'internal server error'})
}

fn main() {
	db_path := os.getenv('SQLITE_PATH')
	if db_path == '' {
		panic('SQLITE_PATH is required')
	}
	mut db := sqlite.connect(db_path) or { panic(err) }
	db.busy_timeout(5000)
	db.exec('PRAGMA journal_mode=WAL') or { panic(err) }
	db.exec('PRAGMA synchronous=NORMAL') or { panic(err) }
	mut app := &App{
		db:      db
		secret:  os.getenv('JWT_SECRET')
		started: time.now().unix()
	}
	port := os.getenv('PORT').int()
	mut server := http.Server{
		addr:                    '${if os.getenv('HOST') == '' { '127.0.0.1' } else { os.getenv('HOST')}}:${if port > 0 { port } else { 3000 }}'
		handler:                 app
		max_keep_alive_requests: 0
		read_timeout:            75 * time.second
		write_timeout:           75 * time.second
	}
	server.listen_and_serve()
}
