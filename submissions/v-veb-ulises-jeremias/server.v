module main

import crypto.hmac
import crypto.sha256
import db.sqlite
import encoding.base64
import json2
import net.http
import os
import runtime
import time
import veb

pub struct Context {
	veb.Context
}

pub fn (mut ctx Context) not_found() veb.Result {
	ctx.res.set_status(.not_found)
	return ctx.json(ErrorResponse{'not found'})
}

pub struct App {
mut:
	db_pool chan sqlite.DB
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

struct CreatePostPayload {
	body json2.Any
}

struct JwtHeader {
	alg string
}

struct JwtClaims {
	exp      json2.Any
	sub      json2.Any
	username json2.Any
}

fn (mut app App) health(mut ctx Context) veb.Result {
	mut db := app.checkout_db()
	defer {
		app.db_pool <- db
	}
	db.q_int('SELECT 1') or {
		ctx.res.set_status(.service_unavailable)
		return ctx.json(DegradedHealthResponse{'degraded', 'unreachable', 'database query failed'})
	}
	return ctx.json(HealthResponse{'ok', 'ok', time.now().unix() - app.started})
}

@[get]
pub fn (mut app App) feed(mut ctx Context) veb.Result {
	mut db := app.checkout_db()
	defer {
		app.db_pool <- db
	}
	rows := db.exec('SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20') or {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	mut posts := []Post{cap: rows.len}
	for row in rows {
		posts << post_from_row(row)
	}
	return ctx.json(FeedResponse{posts})
}

@['/posts/:id'; get]
pub fn (mut app App) get_post(mut ctx Context, id string) veb.Result {
	post_id := parse_positive_id(id)
	if post_id == 0 {
		return app.json_error(mut ctx, .bad_request, 'invalid post id')
	}
	mut db := app.checkout_db()
	defer {
		app.db_pool <- db
	}
	rows := db.exec_param('SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?', post_id.str()) or {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	if rows.len == 0 {
		return app.json_error(mut ctx, .not_found, 'post not found')
	}
	return ctx.json(PostResponse{post_from_row(rows[0])})
}

@['/posts'; post]
pub fn (mut app App) create_post(mut ctx Context) veb.Result {
	user_id, username, auth_error := app.authenticate(ctx.req)
	if auth_error != '' {
		return app.json_error(mut ctx, .unauthorized, auth_error)
	}
	payload := json2.decode[CreatePostPayload](ctx.req.data, json2.DecoderOptions{}) or {
		return app.json_error(mut ctx, .bad_request, 'malformed JSON body')
	}
	body_value := payload.body
	if body_value !is string {
		return app.json_error(mut ctx, .bad_request, 'body is required')
	}
	body := body_value.str().trim_space()
	if body.len == 0 {
		return app.json_error(mut ctx, .bad_request, 'body is required')
	}
	if body.runes().len > 500 {
		return app.json_error(mut ctx, .bad_request, 'body must be at most 500 characters')
	}
	mut db := app.checkout_db()
	defer {
		app.db_pool <- db
	}
	rows := db.exec_param2('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at', user_id.str(), body) or {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	if rows.len == 0 {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	ctx.res.set_status(.created)
	post := Post{rows[0].get_int('id'), body, rows[0].get_string('created_at'), username, 0}
	return ctx.json(PostResponse{post})
}

@['/posts/:id/like'; post]
pub fn (mut app App) like_post(mut ctx Context, id string) veb.Result {
	user_id, _, auth_error := app.authenticate(ctx.req)
	if auth_error != '' {
		return app.json_error(mut ctx, .unauthorized, auth_error)
	}
	post_id := parse_positive_id(id)
	if post_id == 0 {
		return app.json_error(mut ctx, .bad_request, 'invalid post id')
	}
	mut db := app.checkout_db()
	defer {
		app.db_pool <- db
	}
	exists := db.q_int('SELECT EXISTS(SELECT 1 FROM posts WHERE id = ${post_id})') or {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	if exists == 0 {
		return app.json_error(mut ctx, .not_found, 'post not found')
	}
	db.exec_param2('INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING', user_id.str(), post_id.str()) or {
		return app.json_error(mut ctx, .internal_server_error, 'internal server error')
	}
	if db.get_affected_rows_count() > 0 {
		ctx.res.set_status(.created)
		return ctx.json(LikeResponse{true, false, post_id})
	}
	return ctx.json(LikeResponse{true, true, post_id})
}

fn (mut app App) checkout_db() sqlite.DB {
	return <-app.db_pool or { panic('SQLite connection pool is closed') }
}

fn (app &App) authenticate(req http.Request) (int, string, string) {
	authorization := req.header.get(.authorization) or { '' }
	if !authorization.starts_with('Bearer ') {
		return 0, '', 'missing bearer token'
	}
	parts := authorization['Bearer '.len..].split('.')
	if parts.len != 3 || !valid_base64url(parts[0]) || !valid_base64url(parts[1]) || !valid_base64url(parts[2]) {
		return 0, '', 'invalid or expired token'
	}
	header := json2.decode[JwtHeader](base64.url_decode_str(parts[0]), json2.DecoderOptions{}) or {
		return 0, '', 'invalid or expired token'
	}
	if header.alg != 'HS256' {
		return 0, '', 'invalid or expired token'
	}
	expected := hmac.new(app.secret.bytes(), '${parts[0]}.${parts[1]}'.bytes(), sha256.sum, sha256.block_size)
	actual := base64.url_decode(parts[2])
	if !hmac.equal(expected, actual) {
		return 0, '', 'invalid or expired token'
	}
	claims := json2.decode[JwtClaims](base64.url_decode_str(parts[1]), json2.DecoderOptions{}) or {
		return 0, '', 'invalid or expired token'
	}
	expiration := claims.exp
	if !is_number(expiration) || expiration.i64() <= time.now().unix() {
		return 0, '', 'invalid or expired token'
	}
	subject := claims.sub
	username := claims.username
	if subject !is string || username !is string {
		return 0, '', 'invalid token payload'
	}
	user_id := parse_positive_id(subject.str())
	if user_id == 0 {
		return 0, '', 'invalid token payload'
	}
	return user_id, username.str(), ''
}

fn (app &App) json_error(mut ctx Context, status int, message string) veb.Result {
	ctx.res.set_status(http.status_from_int(status))
	return ctx.json(ErrorResponse{message})
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

fn main() {
	db_path := os.getenv('SQLITE_PATH')
	if db_path == '' {
		panic('SQLITE_PATH is required')
	}
	pool_size := if runtime.nr_cpus() > 4 { 4 } else { runtime.nr_cpus() }
	mut db_pool := chan sqlite.DB{cap: pool_size}
	for i in 0 .. pool_size {
		mut db := sqlite.connect(db_path) or { panic(err) }
		db.busy_timeout(5000)
		if i == 0 {
			db.exec('PRAGMA journal_mode=WAL') or { panic(err) }
		}
		db.exec('PRAGMA synchronous=NORMAL') or { panic(err) }
		// Keep the SQLite page cache bounded while allowing hot indexed pages to
		// avoid repeated reads from the database file. mmap_size is per connection.
		db.exec('PRAGMA cache_size=-32768') or { panic(err) }
		db.exec('PRAGMA mmap_size=1073741824') or { panic(err) }
		db_pool <- db
	}
	mut app := &App{
		db_pool: db_pool
		secret:  os.getenv('JWT_SECRET')
		started: time.now().unix()
	}
	host := if os.getenv('HOST') == '' { '127.0.0.1' } else { os.getenv('HOST') }
	port := os.getenv('PORT').int()
	veb.run_at[App, Context](mut app, host: host, port: if port > 0 { port } else { 3000 }, family: .ip,
		timeout_in_seconds: 75) or { panic(err) }
}
