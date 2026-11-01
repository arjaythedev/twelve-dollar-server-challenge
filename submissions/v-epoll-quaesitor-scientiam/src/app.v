module main

import fasthttp
import os
import time

const max_body_chars = 500
const feed_span = 256 // feed_by_range handles the newest 20 ids when they span fewer ids than this

// App is everything one event loop needs to answer requests: the database connection,
// the token verifier and the scratch buffers every request reuses. Handlers append the
// full HTTP response to `out` and never allocate on the steady-state path.
@[heap]
struct App {
mut:
	db      &Db = unsafe { nil }
	secret  []u8
	sig     []u8 // decoded token signature
	scratch []u8 // decoded JOSE header, then payload
	text    []u8 // decoded request body / `sub`
	user    []u8 // decoded `username`
	body    []u8 // response body being built
	rows    []u8 // feed_by_range: post objects in id order before reordering
	started i64
	// feed_range turns the three-scan feed on (FEED_RANGE=0 in the environment turns it off)
	feed_range bool = true
	now        i64 // unix seconds, refreshed by the backend once per batch
}

// new_app builds an App from the four environment variables in SPEC.md.
fn new_app(group bool, exclusive bool) !&App {
	path := os.getenv('SQLITE_PATH')
	if path == '' {
		return error('SQLITE_PATH is not set')
	}
	mut app := &App{
		secret:  os.getenv('JWT_SECRET').bytes()
		sig:     []u8{cap: 64}
		scratch: []u8{cap: 1024}
		text:    []u8{cap: 4096}
		user:    []u8{cap: 256}
		body:    []u8{cap: 16384}
		rows:    []u8{cap: 16384}
		started: time.now().unix()
	}
	if os.getenv('FEED_RANGE') == '0' {
		app.feed_range = false
	}
	app.db = open_db(path, group, exclusive)!
	app.tick()
	return app
}

// tick refreshes the cached wall clock; backends call it once per event batch.
fn (mut app App) tick() {
	app.now = time.now().unix()
}

@[inline]
fn (app &App) now_unix() i64 {
	return app.now
}

// batch_end commits the batch transaction and runs a due checkpoint. It returns false
// when the commit failed; the backend then replaces the batch's write responses.
fn (mut app App) batch_end() bool {
	ok := app.db.commit()
	app.db.checkpoint()
	return ok
}

// Outcome tells the backend what the handler did with the connection and the database.
struct Outcome {
	wrote bool // a statement that changes the database ran (its response needs the commit)
	close bool
}

// handle routes one request and appends its response to out.
fn (mut app App) handle(req Req, mut out []u8) Outcome {
	buf := req.buffer
	is_get := bytes_eq(buf, req.method, 'GET')
	is_post := !is_get && bytes_eq(buf, req.method, 'POST')
	mut p_start := req.path.start
	mut p_end := req.path.start + req.path.len
	for k in p_start .. p_end {
		if buf[k] == `?` {
			p_end = k
			break
		}
	}
	plen := p_end - p_start
	path := fasthttp.Slice{
		start: p_start
		len:   plen
	}
	if is_get {
		if plen == 5 && bytes_eq(buf, path, '/feed') {
			app.feed(mut out)
			return Outcome{}
		}
		if plen == 7 && bytes_eq(buf, path, '/health') {
			app.health(mut out)
			return Outcome{}
		}
		if plen > 7 && path_prefix(buf, p_start, '/posts/') {
			id := parse_post_id(buf, p_start + 7, p_end)
			if id < 0 {
				put_error(mut out, status_400, 'invalid post id', false)
				return Outcome{}
			}
			app.post(id, mut out)
			return Outcome{}
		}
	} else if is_post {
		if plen == 6 && bytes_eq(buf, path, '/posts') {
			return app.create(req, mut out)
		}
		if plen > 12 && path_prefix(buf, p_start, '/posts/') && path_suffix(buf, p_end, '/like') {
			return app.like(req, p_start + 7, p_end - 5, mut out)
		}
	}
	put_error(mut out, status_404, 'not found', false)
	return Outcome{}
}

fn path_prefix(buf []u8, start int, lit string) bool {
	for k in 0 .. lit.len {
		if buf[start + k] != lit[k] {
			return false
		}
	}
	return true
}

fn path_suffix(buf []u8, end int, lit string) bool {
	for k in 0 .. lit.len {
		if buf[end - lit.len + k] != lit[k] {
			return false
		}
	}
	return true
}

fn (mut app App) health(mut out []u8) {
	app.body.clear()
	st := app.db.st_health
	rc := C.sqlite3_step(st)
	C.sqlite3_reset(st)
	if rc == sqlite_row {
		put(mut app.body, '{"status":"ok","db":"ok","uptime_s":')
		json_int(mut app.body, app.now - app.started)
		put(mut app.body, '}')
		put_response(mut out, status_200, app.body, false)
		return
	}
	put(mut app.body, '{"status":"degraded","db":"unreachable","error":')
	msg := app.db.errmsg()
	json_str(mut app.body, msg.str, msg.len)
	put(mut app.body, '}')
	put_response(mut out, status_503, app.body, false)
}

fn (mut app App) feed(mut out []u8) {
	app.db.begin()
	app.body.clear()
	if app.feed_range && app.feed_by_range() {
		put_response(mut out, status_200, app.body, false)
		return
	}
	app.body.clear()
	put(mut app.body, '{"posts":[')
	st := app.db.st_feed
	mut n := 0
	mut rc := C.sqlite3_step(st)
	for rc == sqlite_row {
		if n > 0 {
			app.body << `,`
		}
		json_post_row(mut app.body, st)
		n++
		rc = C.sqlite3_step(st)
	}
	C.sqlite3_reset(st)
	if rc != sqlite_done {
		put_error(mut out, status_500, 'internal server error', false) // a step error, not "no rows"
		return
	}
	put(mut app.body, ']}')
	put_response(mut out, status_200, app.body, false)
}

// feed_by_range builds the feed from three scans when the newest 20 ids span fewer than
// feed_span ids: the ids, the likes in that id range, the post rows in that id range. The
// rows come back in id order and are put back into feed order by position. It returns
// false (nothing written) when the ids are sparse or the row count does not match, and
// the caller runs the reference query instead. Every value still comes from SQLite now.
fn (mut app App) feed_by_range() bool {
	mut ids := [20]i64{}
	mut n := 0
	st := app.db.st_ids
	mut rc := C.sqlite3_step(st)
	for n < 20 && rc == sqlite_row {
		ids[n] = C.sqlite3_column_int64(st, 0)
		n++
		rc = C.sqlite3_step(st)
	}
	C.sqlite3_reset(st)
	if rc != sqlite_row && rc != sqlite_done {
		return false // a step error: the reference query reports it
	}
	if n == 0 {
		put(mut app.body, '{"posts":[]}')
		return true
	}
	mut lo := ids[0]
	mut hi := ids[0]
	for i in 1 .. n {
		if ids[i] < lo {
			lo = ids[i]
		}
		if ids[i] > hi {
			hi = ids[i]
		}
	}
	if hi - lo >= feed_span {
		return false
	}
	mut slot := [feed_span]i8{init: -1}
	mut likes := [20]i64{}
	mut seg_start := [20]int{}
	mut seg_end := [20]int{}
	for i in 0 .. n {
		slot[int(ids[i] - lo)] = i8(i)
	}
	lk := app.db.st_likes
	C.sqlite3_bind_int64(lk, 1, lo)
	C.sqlite3_bind_int64(lk, 2, hi)
	rc = C.sqlite3_step(lk)
	for rc == sqlite_row {
		p := slot[int(C.sqlite3_column_int64(lk, 0) - lo)]
		if p >= 0 {
			likes[p]++
		}
		rc = C.sqlite3_step(lk)
	}
	C.sqlite3_reset(lk)
	if rc != sqlite_done {
		return false
	}
	rw := app.db.st_rows
	C.sqlite3_bind_int64(rw, 1, lo)
	C.sqlite3_bind_int64(rw, 2, hi)
	app.rows.clear()
	mut matched := 0
	rc = C.sqlite3_step(rw)
	for rc == sqlite_row {
		id := C.sqlite3_column_int64(rw, 0)
		p := slot[int(id - lo)]
		if p >= 0 {
			seg_start[p] = app.rows.len
			json_post(mut app.rows, id, C.sqlite3_column_text(rw, 1), C.sqlite3_column_bytes(rw, 1),
				C.sqlite3_column_text(rw, 2), C.sqlite3_column_bytes(rw, 2),
				C.sqlite3_column_text(rw, 3), C.sqlite3_column_bytes(rw, 3), likes[p])
			seg_end[p] = app.rows.len
			matched++
		}
		rc = C.sqlite3_step(rw)
	}
	C.sqlite3_reset(rw)
	if rc != sqlite_done || matched != n {
		return false
	}
	put(mut app.body, '{"posts":[')
	for i in 0 .. n {
		if i > 0 {
			app.body << `,`
		}
		put_bytes(mut app.body, unsafe { &u8(app.rows.data) + seg_start[i] }, seg_end[i] - seg_start[i])
	}
	put(mut app.body, ']}')
	return true
}

fn (mut app App) post(id i64, mut out []u8) {
	app.db.begin()
	st := app.db.st_post
	C.sqlite3_bind_int64(st, 1, id)
	rc := C.sqlite3_step(st)
	if rc == sqlite_row {
		app.body.clear()
		put(mut app.body, '{"post":')
		json_post_row(mut app.body, st)
		app.body << `}`
		C.sqlite3_reset(st)
		put_response(mut out, status_200, app.body, false)
		return
	}
	C.sqlite3_reset(st)
	if rc != sqlite_done {
		put_error(mut out, status_500, 'internal server error', false) // a step error, not "no rows"
		return
	}
	put_error(mut out, status_404, 'post not found', false)
}

// authenticate verifies the bearer token, writing the 401 itself when it fails.
fn (mut app App) authenticate(req Req, mut out []u8) ?Claims {
	tok := bearer_token(req.buffer, req.header_fields)
	if tok.len < 0 {
		put_error(mut out, status_401, 'missing bearer token', false)
		return none
	}
	claims := app.verify_token(req.buffer[tok.start..tok.start + tok.len]) or {
		if err.code() == int(AuthError.payload) {
			put_error(mut out, status_401, 'invalid token payload', false)
		} else {
			put_error(mut out, status_401, 'invalid or expired token', false)
		}
		return none
	}
	return claims
}

@[inline]
fn is_trim_ws(c u8) bool {
	return c == ` ` || c == `\t` || c == `\n` || c == `\r` || c == `\f` || c == `\v`
}

// read_body_text parses `{"body":"..."}` from the request body into app.text (trimmed).
// It writes the 400 response itself when the body is unusable.
fn (mut app App) read_body_text(req Req, mut out []u8) bool {
	raw := req.buffer[req.body.start..req.body.start + req.body.len]
	mut w := new_object_walker(raw) or {
		put_error(mut out, status_400, 'malformed JSON body', false)
		return false
	}
	mut found := false
	app.text.clear()
	for {
		more := w.next() or {
			put_error(mut out, status_400, 'malformed JSON body', false)
			return false
		}
		if !more {
			break
		}
		if w.key('body') && w.value_is_string() {
			w.read_string(mut app.text) or {
				put_error(mut out, status_400, 'malformed JSON body', false)
				return false
			}
			found = true
		} else {
			if w.key('body') {
				found = false
			}
			w.skip() or {
				put_error(mut out, status_400, 'malformed JSON body', false)
				return false
			}
		}
	}
	if !found {
		put_error(mut out, status_400, 'body is required', false)
		return false
	}
	// trim ASCII whitespace in place
	mut s := 0
	mut e := app.text.len
	for s < e && is_trim_ws(app.text[s]) {
		s++
	}
	for e > s && is_trim_ws(app.text[e - 1]) {
		e--
	}
	if e == s {
		put_error(mut out, status_400, 'body is required', false)
		return false
	}
	if s > 0 {
		unsafe { vmemmove(app.text.data, &u8(app.text.data) + s, e - s) }
	}
	unsafe {
		app.text.len = e - s
	}
	mut chars := 0
	for c in app.text {
		if c & 0xC0 != 0x80 {
			chars++
		}
	}
	if chars > max_body_chars {
		put_error(mut out, status_400, 'body must be at most 500 characters', false)
		return false
	}
	return true
}

fn (mut app App) create(req Req, mut out []u8) Outcome {
	claims := app.authenticate(req, mut out) or { return Outcome{} }
	if !app.read_body_text(req, mut out) {
		return Outcome{}
	}
	app.db.begin()
	st := app.db.st_insert
	C.sqlite3_bind_int64(st, 1, claims.user_id)
	C.sqlite3_bind_text(st, 2, &char(app.text.data), app.text.len, unsafe { nil })
	if !run_once(st) {
		put_error(mut out, status_500, 'internal server error', false)
		return Outcome{
			wrote: true
		}
	}
	id := C.sqlite3_last_insert_rowid(app.db.conn)
	cr := app.db.st_created
	C.sqlite3_bind_int64(cr, 1, id)
	if C.sqlite3_step(cr) != sqlite_row {
		C.sqlite3_reset(cr)
		put_error(mut out, status_500, 'internal server error', false)
		return Outcome{
			wrote: true
		}
	}
	app.body.clear()
	put(mut app.body, '{"post":')
	json_post(mut app.body, id, app.text.data, app.text.len, C.sqlite3_column_text(cr, 0),
		C.sqlite3_column_bytes(cr, 0), claims.username.data, claims.username.len, 0)
	app.body << `}`
	C.sqlite3_reset(cr)
	put_response(mut out, status_201, app.body, false)
	return Outcome{
		wrote: true
	}
}

fn (mut app App) like(req Req, id_start int, id_end int, mut out []u8) Outcome {
	claims := app.authenticate(req, mut out) or { return Outcome{} }
	id := parse_post_id(req.buffer, id_start, id_end)
	if id < 0 {
		put_error(mut out, status_400, 'invalid post id', false)
		return Outcome{}
	}
	app.db.begin()
	st := app.db.st_like
	C.sqlite3_bind_int64(st, 1, claims.user_id)
	C.sqlite3_bind_int64(st, 2, id)
	if !run_once(st) {
		put_error(mut out, status_500, 'internal server error', false)
		return Outcome{
			wrote: true
		}
	}
	mut already := false
	if C.sqlite3_changes(app.db.conn) != 1 {
		ex := app.db.st_exists
		C.sqlite3_bind_int64(ex, 1, id)
		rc := C.sqlite3_step(ex)
		C.sqlite3_reset(ex)
		if rc == sqlite_done {
			put_error(mut out, status_404, 'post not found', false)
			return Outcome{}
		}
		if rc != sqlite_row {
			put_error(mut out, status_500, 'internal server error', false)
			return Outcome{}
		}
		already = true
	}
	app.body.clear()
	put(mut app.body, '{"liked":true,"already_liked":')
	put(mut app.body, if already { 'true' } else { 'false' })
	put(mut app.body, ',"post_id":')
	json_int(mut app.body, id)
	app.body << `}`
	put_response(mut out, if already { status_200 } else { status_201 }, app.body, false)
	return Outcome{
		wrote: !already
	}
}
