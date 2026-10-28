// One thread owns the io_uring rings and the request SQLite connection.
// Responses enter the transport only after the batch transaction commits.
module main

import net
import os
import time

#flag -I @DIR/build
#flag @DIR/build/sqlite3.o
#flag -I @DIR
#flag @DIR/build/uring_bridge.o
#flag @DIR/build/liburing/src/liburing.a
#flag -lm -lcrypto
#include "uring_bridge.h"
#include "json_scan.h"
#include <openssl/hmac.h>
#include "sqlite3.h"
#include "ckpt.h"
#include <sys/resource.h>
#include <sys/eventfd.h>

struct C.sqlite3 {}

struct C.sqlite3_stmt {}

fn C.sqlite3_open_v2(&char, &&C.sqlite3, i32, &char) i32
fn C.sqlite3_exec(&C.sqlite3, &char, voidptr, voidptr, &&char) i32
fn C.sqlite3_prepare_v3(&C.sqlite3, &char, i32, u32, &&C.sqlite3_stmt, voidptr) i32
fn C.sqlite3_bind_int64(&C.sqlite3_stmt, i32, i64) i32
fn C.sqlite3_bind_text(&C.sqlite3_stmt, i32, &char, i32, voidptr) i32
fn C.sqlite3_step(&C.sqlite3_stmt) i32
fn C.sqlite3_reset(&C.sqlite3_stmt) i32
fn C.sqlite3_column_int64(&C.sqlite3_stmt, i32) i64
fn C.sqlite3_column_text(&C.sqlite3_stmt, i32) &u8
fn C.sqlite3_column_bytes(&C.sqlite3_stmt, i32) i32
fn C.sqlite3_changes(&C.sqlite3) i32
fn C.sqlite3_errmsg(&C.sqlite3) &char
fn C.sqlite3_db_config(&C.sqlite3, i32, i32, voidptr) i32
fn C.HMAC(voidptr, voidptr, i32, &u8, usize, &u8, &u32) &u8
fn C.sqlite3_wal_hook(&C.sqlite3, voidptr, voidptr) voidptr
fn C.eventfd(u32, i32) i32
fn C.td_ckpt_start(&char, int, int)
fn C.sqlite3_wal_checkpoint_v2(&C.sqlite3, &char, i32, &i32, &i32) i32
fn C.EVP_sha256() voidptr

struct C.rlimit {
mut:
	rlim_cur u64
	rlim_max u64
}

struct C.td_net {}

struct C.td_event {
	kind   int
	id     int
	data   usize // C owns these receive buffers; the GC must not scan the event array.
	len    int
	buffer int
}

fn C.td_net_new(int, int) &C.td_net
fn C.td_net_next(&C.td_net, &C.td_event, int, int) int
fn C.td_net_release(&C.td_net, &C.td_event)
fn C.td_net_send(&C.td_net, int, &u8, int)
fn C.td_net_resume(&C.td_net, int, int, int)
fn C.td_net_abort(&C.td_net, int)
fn C.td_json_scan(&u8, int) int
fn C.memchr(voidptr, int, usize) voidptr
fn C.getrlimit(i32, &C.rlimit) i32
fn C.setrlimit(i32, &C.rlimit) i32

const sqlite_row = 100
const sqlite_done = 101
const max_events = 1024
const max_request = 65536
const header_room = 128 // fits the longest response header (36 + 19 + 48 + 10 + 4 bytes)

const sql_post = 'SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id'

const feed_span = 64 // feed_range handles the newest 20 ids when they span fewer ids than this
const ckpt_frames = 1000 // WAL size that starts a checkpoint (SQLite's default auto-checkpoint)
const ckpt_max_frames = 16000 // past this WAL size, phase B holds writes until the WAL is copied
const max_pause_ms = u64(20) // longest hold on write requests while the WAL can restart
const write_window_ns = u64(5_000_000) // after a commit that wrote, hold new writes this long to group them

struct Conn {
mut:
	inbuf       []u8 // Only partial or deferred requests retain an allocation.
	close_after bool
	queued      bool
	open        bool
	ready       bool // The transport holds no operation for this connection.
	deferred    bool
	has_output  bool
}

struct Seg {
mut:
	fd    int // -1 until after_parse assigns the connection
	start int
	end   int
}

struct Server {
mut:
	db        &C.sqlite3      = unsafe { nil }
	st_feed   &C.sqlite3_stmt = unsafe { nil }
	st_top    &C.sqlite3_stmt = unsafe { nil }
	st_range  &C.sqlite3_stmt = unsafe { nil }
	st_counts &C.sqlite3_stmt = unsafe { nil }
	st_post   &C.sqlite3_stmt = unsafe { nil }
	st_exists &C.sqlite3_stmt = unsafe { nil }
	st_insert &C.sqlite3_stmt = unsafe { nil }
	st_like   &C.sqlite3_stmt = unsafe { nil }
	st_health &C.sqlite3_stmt = unsafe { nil }
	st_begin  &C.sqlite3_stmt = unsafe { nil }
	st_commit &C.sqlite3_stmt = unsafe { nil }
	st_back   &C.sqlite3_stmt = unsafe { nil }
	in_txn    bool
	secret    []u8
	started   i64
	transport &C.td_net = unsafe { nil }
	conns     []Conn
	out       []u8 // all responses of the current batch
	body_start int = -1 // where the current response body starts in out; -1 when none is open
	head      []u8 // scratch for one response header
	jkey      []u8 // the decoded key of the current JSON member of a post body
	jbody     []u8 // the decoded "body" member of the current post
	segs      []Seg
	flush     []int
	cur_close bool
	// Cooperative checkpoint: see ckpt.h and checkpoint_done.
	wal_frames    int
	wal_written   bool // the current batch's COMMIT appended WAL frames
	ckpt_frames_at_start int // WAL size when phase A started; more frames afterwards block a restart
	ckpt_phase    int // 0 idle, 1 checkpoint with writes going on, 2 checkpoint with writes held
	writes_paused bool
	pause_until   u64 // monotonic ns when held writes go on anyway (0: wait for the checkpoint)
	ckpt_req      int // eventfd: each write starts one checkpoint on the checkpoint thread
	ckpt_efd      int // eventfd: the checkpoint thread answers on it
	deferring     bool // set by parse when it stops at a write request
	deferred      []int
}

fn main() {
	mut s := &Server{
		secret:  os.getenv('JWT_SECRET').bytes()
		started: time.now().unix()
		out:     []u8{cap: 1 << 20}
		head:    []u8{cap: header_room}
		jkey:    []u8{cap: 64}
		jbody:   []u8{cap: 2048}
		conns:   []Conn{len: int(C.TD_CAPACITY)}
	}
	path := os.getenv('SQLITE_PATH')
	s.open_db(path)
	s.ckpt_efd = C.eventfd(0, C.EFD_NONBLOCK | C.EFD_CLOEXEC)
	s.ckpt_req = C.eventfd(0, C.EFD_CLOEXEC)
	C.td_ckpt_start(&char(path.str), s.ckpt_req, s.ckpt_efd)
	mut rl := C.rlimit{}
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	rl.rlim_cur = rl.rlim_max
	C.setrlimit(C.RLIMIT_NOFILE, &rl)
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	if rl.rlim_cur < 65535 {
		panic('io_uring requires a process descriptor limit of at least 65535')
	}
	host := os.getenv_opt('HOST') or { '0.0.0.0' }
	port := os.getenv_opt('PORT') or { '3000' }
	mut l := net.listen_tcp(.ip, '${host}:${port}', backlog: 65535) or { panic(err) }
	s.run(int(l.sock.handle))
}

fn (s &Server) request_checkpoint() {
	one := u64(1)
	C.write(s.ckpt_req, &one, 8)
}

// wal_hook runs inside each COMMIT and records the WAL size in frames.
fn wal_hook(arg voidptr, _ &C.sqlite3, _ &char, frames i32) i32 {
	mut s := unsafe { &Server(arg) }
	s.wal_frames = frames
	s.wal_written = true
	return 0
}

fn (mut s Server) open_db(path string) {
	if C.sqlite3_open_v2(&char(path.str), &s.db, 2, unsafe { nil }) != 0 {
		panic('cannot open ${path}')
	}
	// Query planner stability: without it, SQLite re-prepares the post query on every new
	// bound id, because the id is compared with p.id inside the correlated like count.
	C.sqlite3_db_config(s.db, C.SQLITE_DBCONFIG_ENABLE_QPSG, 1, unsafe { nil })
	C.sqlite3_exec(s.db,
		c'PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=1000; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY',
		unsafe { nil }, unsafe { nil }, unsafe { nil })
	// Replaces SQLite's inline auto-checkpoint: checkpoints run on the checkpointer thread.
	C.sqlite3_wal_hook(s.db, wal_hook, s)
	s.st_feed = s.prepare(sql_post + ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20')
	s.st_top = s.prepare('SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20')
	s.st_range =
		s.prepare('SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ? ORDER BY p.id DESC')
	s.st_counts = s.prepare('SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?')
	s.st_post = s.prepare(sql_post + ' WHERE p.id = ?')
	s.st_exists = s.prepare('SELECT 1 FROM posts WHERE id = ?')
	s.st_insert =
		s.prepare('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at')
	s.st_like =
		s.prepare('INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING')
	s.st_health = s.prepare('SELECT 1')
	s.st_begin = s.prepare('BEGIN')
	s.st_commit = s.prepare('COMMIT')
	s.st_back = s.prepare('ROLLBACK')
}

fn (mut s Server) prepare(query string) &C.sqlite3_stmt {
	mut st := &C.sqlite3_stmt(unsafe { nil })
	if C.sqlite3_prepare_v3(s.db, &char(query.str), query.len, 1, &st, unsafe { nil }) != 0 {
		panic(unsafe { cstring_to_vstring(C.sqlite3_errmsg(s.db)) })
	}
	return st
}

// A failed BEGIN must not let later statements run in auto-commit mode.
fn (mut s Server) txn() {
	if !s.in_txn {
		if !run_once(s.st_begin) {
			panic('cannot begin request transaction')
		}
		s.in_txn = true
	}
}

fn run_once(st &C.sqlite3_stmt) bool {
	rc := C.sqlite3_step(st)
	C.sqlite3_reset(st)
	return rc == sqlite_done
}

fn (mut s Server) run(lfd int) {
	s.transport = C.td_net_new(lfd, s.ckpt_efd)
	mut events := []C.td_event{len: max_events}
	for {
		n := C.td_net_next(s.transport, events.data, max_events, s.pause_timeout())
		if s.writes_paused && s.pause_until > 0 && time.sys_mono_now() >= s.pause_until {
			s.resume_writes()
		}
		for i in 0 .. n {
			event := events[i]
			fd := event.id
			match event.kind {
				1 {
					s.conns[fd] = Conn{
						open:  true
						ready: true
					}
					s.queue(fd)
				}
				2 {
					s.conns[fd].ready = true
					s.read_conn(fd, unsafe { &u8(event.data) }, event.len)
				}
				3 {
					s.conns[fd] = Conn{}
				}
				4 {
					s.output_drained(fd)
				}
				5 {
					s.checkpoint_done()
				}
				else {
					panic('invalid transport event')
				}
			}

			C.td_net_release(s.transport, &event)
		}
		s.finish_batch()
	}
}

fn (mut s Server) queue(fd int) {
	if !s.conns[fd].queued {
		s.conns[fd].queued = true
		s.flush << fd
	}
}

fn (mut s Server) output_drained(fd int) {
	s.conns[fd].ready = true
	if !s.conns[fd].deferred || !s.writes_paused {
		s.conns[fd].deferred = false
		s.read_conn(fd, unsafe { nil }, 0)
	}
}

fn (mut s Server) read_conn(fd int, data &u8, n int) {
	start := s.segs.len
	s.cur_close = s.conns[fd].close_after
	if s.cur_close {
		s.queue(fd)
		return
	}
	if s.conns[fd].inbuf.len == 0 && n > 0 {
		buf := unsafe { data.vbytes(n) }
		used := s.parse(buf, n)
		if used < n && !s.cur_close {
			s.conns[fd].inbuf = buf[used..].clone()
		}
	} else if s.conns[fd].inbuf.len > 0 {
		if n > 0 {
			unsafe { s.conns[fd].inbuf.push_many(data, n) }
		}
		buf := s.conns[fd].inbuf
		used := s.parse(buf, buf.len)
		s.conns[fd].inbuf = if used < buf.len && !s.cur_close {
			buf[used..].clone()
		} else {
			[]u8{}
		}
	}
	if s.conns[fd].inbuf.len > max_request {
		s.cur_close = true
		s.error(400, 'request is too large')
	}
	s.after_parse(fd, start)
}

// after_parse queues what parse produced for fd: responses (segments from index start on), a close,
// or a deferred write request.
fn (mut s Server) after_parse(fd int, start int) {
	if s.deferring {
		s.deferring = false
		if !s.conns[fd].deferred {
			s.conns[fd].deferred = true
			s.deferred << fd
		}
	}
	if s.segs.len > start {
		for i in start .. s.segs.len {
			s.segs[i].fd = fd
		}
		s.conns[fd].has_output = true
	}
	if s.cur_close {
		s.conns[fd].close_after = true
	}
	s.queue(fd)
}

fn (mut s Server) pause_timeout() int {
	if !s.writes_paused || s.pause_until == 0 {
		return -1
	}
	left := i64(s.pause_until) - i64(time.sys_mono_now())
	return if left <= 0 { 0 } else { int(left / 1_000_000) + 1 }
}

// The loop starts a checkpoint (phase A) when the WAL reaches ckpt_frames; writes go on. A PASSIVE
// checkpoint copies only the frames present when it starts, so if it could not copy them all, or
// writes appended frames meanwhile, the WAL cannot restart. Then phase B holds write requests
// (reads go on) and runs one more checkpoint, which only copies the frames appended during phase A.
// Phase B ends when that checkpoint finishes or after max_pause_ms, whichever is first; past
// ckpt_max_frames it waits, and so does phase A (finish_batch holds writes as soon as the WAL reaches
// that size). checkpoint_done runs when the checkpoint thread signals the end of a checkpoint.
fn (mut s Server) checkpoint_done() {
	mut v := u64(0)
	C.read(s.ckpt_efd, &v, 8)
	if s.ckpt_phase == 1 && (v != 1 || s.wal_frames > s.ckpt_frames_at_start) {
		s.ckpt_phase = 2
		s.writes_paused = true
		s.pause_until = if s.wal_frames >= ckpt_max_frames {
			u64(0)
		} else {
			time.sys_mono_now() + max_pause_ms * 1_000_000
		}
		s.request_checkpoint()
		return
	}
	s.ckpt_phase = 0
	s.resume_writes()
}

// resume_writes ends phase B and answers the write requests it held.
fn (mut s Server) resume_writes() {
	if !s.writes_paused {
		return
	}
	s.writes_paused = false
	s.pause_until = 0
	for fd in s.deferred {
		mut c := &s.conns[fd]
		was_deferred := c.deferred
		c.deferred = false
		if !c.open || !was_deferred || !c.ready || c.inbuf.len == 0 {
			continue
		}
		start := s.segs.len
		s.cur_close = c.close_after
		buf := c.inbuf
		used := s.parse(buf, buf.len)
		s.conns[fd].inbuf = if used < buf.len { buf[used..].clone() } else { []u8{} }
		s.after_parse(fd, start)
	}
	s.deferred.clear()
}

// No response, including a read of a same-batch write, reaches C before COMMIT.
fn (mut s Server) finish_batch() {
	mut committed := true
	if s.in_txn {
		s.in_txn = false
		committed = run_once(s.st_commit)
		if !committed {
			run_once(s.st_back)
			for seg in s.segs {
				s.conns[seg.fd].close_after = true
				s.conns[seg.fd].deferred = false
			}
		}
	}
	// Group commit: each committing batch rewrites the same hot pages into the WAL, so a short hold
	// on later writes cuts WAL frames, checkpoint copies, and commits. Reads go on.
	if s.wal_written && !s.writes_paused {
		s.writes_paused = true
		s.pause_until = time.sys_mono_now() + write_window_ns
	}
	s.wal_written = false
	if s.ckpt_phase != 0 && s.wal_frames >= ckpt_max_frames {
		// A slow disk must not let the WAL (and its dirty pages) grow without bound: hold writes until
		// the checkpoint that is running ends, whichever phase it is in.
		s.writes_paused = true
		s.pause_until = 0
	}
	if s.wal_frames >= ckpt_frames && s.ckpt_phase == 0 {
		s.ckpt_phase = 1
		s.ckpt_frames_at_start = s.wal_frames
		s.request_checkpoint()
	}
	if committed {
		for seg in s.segs {
			C.td_net_send(s.transport, seg.fd, unsafe { &s.out[seg.start] }, seg.end - seg.start)
		}
	}
	for fd in s.flush {
		mut c := &s.conns[fd]
		c.queued = false
		if !c.ready {
			continue
		}
		if !committed && c.close_after {
			C.td_net_abort(s.transport, fd)
			c.ready = false
			c.has_output = false
			continue
		}
		if c.deferred && !c.has_output {
			continue
		}
		C.td_net_resume(s.transport, fd, if c.close_after { 1 } else { 0 },
			if c.inbuf.len > 0 || c.deferred { 1 } else { 0 })
		c.ready = false
		c.has_output = false
	}
	s.segs.clear()
	s.flush.clear()
	unsafe {
		s.out.len = 0
	}
}

// eq_ci compares case-insensitively with a lowercase literal of letters and `-`.
fn eq_ci(buf []u8, at int, lit string) bool {
	for i in 0 .. lit.len {
		if buf[at + i] | 0x20 != lit[i] {
			return false
		}
	}
	return true
}

fn find_crlf(buf []u8, from int, end int) int {
	mut i := from
	for i + 1 < end {
		p := unsafe { C.memchr(&u8(buf.data) + i, `\n`, usize(end - i)) }
		if p == unsafe { nil } {
			return -1
		}
		j := int(usize(p) - usize(buf.data))
		if j > from && buf[j - 1] == `\r` {
			return j - 1
		}
		i = j + 1
	}
	return -1
}

// parse limits each turn to 64 requests or 64 KiB of output and returns the bytes consumed.
fn (mut s Server) parse(buf []u8, n int) int {
	batch_start := s.out.len
	mut count := 0
	mut pos := 0
	for pos < n {
		line_end := find_crlf(buf, pos, n)
		if line_end < 0 {
			return pos
		}
		// headers
		mut clen := 0
		mut auth_start := -1
		mut auth_end := -1
		mut close := false
		mut h := line_end + 2
		mut hdr_end := -1
		for {
			e := find_crlf(buf, h, n)
			if e < 0 {
				break
			}
			if e == h {
				hdr_end = e + 2
				break
			}
			cp := unsafe { C.memchr(&u8(buf.data) + h, `:`, usize(e - h)) }
			colon := if cp == unsafe { nil } { e } else { int(usize(cp) - usize(buf.data)) }
			mut v := colon + 1
			for v < e && buf[v] == ` ` {
				v++
			}
			name_len := colon - h
			if name_len == 14 && eq_ci(buf, h, 'content-length') {
				if v >= e {
					s.cur_close = true
					s.error(400, 'invalid content length')
					return n
				}
				for j in v .. e {
					digit := int(buf[j]) - int(`0`)
					if digit < 0 || digit > 9 || clen > (max_request - digit) / 10 {
						s.cur_close = true
						s.error(400, 'invalid content length')
						return n
					}
					clen = clen * 10 + digit
				}
			} else if name_len == 13 && eq_ci(buf, h, 'authorization') {
				auth_start = v
				auth_end = e
			} else if name_len == 10 && eq_ci(buf, h, 'connection') {
				close = e - v == 5 && eq_ci(buf, v, 'close')
			}
			h = e + 2
		}
		if hdr_end >= 0 && hdr_end - pos + clen > max_request {
			s.cur_close = true
			s.error(400, 'request is too large')
			return n
		}
		if hdr_end < 0 || hdr_end + clen > n {
			return pos // request not complete yet
		}
		// request line: METHOD SP PATH SP VERSION
		mut sp := pos
		for sp < line_end && buf[sp] != ` ` {
			sp++
		}
		mut pe := sp + 1
		for pe < line_end && buf[pe] != ` ` && buf[pe] != `?` {
			pe++
		}
		is_post := sp - pos == 4 && buf[pos] == `P`
		if is_post && s.writes_paused {
			s.deferring = true // keep this request and the ones after it until the checkpoint ends
			return pos
		}
		path := unsafe { tos(&buf[0] + sp + 1, pe - sp - 1) }
		if close {
			s.cur_close = true
		}
		s.route(is_post, path, buf, auth_start, auth_end, hdr_end, hdr_end + clen)
		pos = hdr_end + clen
		count++
		if s.cur_close || count >= 64 || s.out.len - batch_start >= 65536 {
			return pos
		}
	}
	return pos
}

fn (mut s Server) route(is_post bool, path string, buf []u8, auth_start int, auth_end int, body_start int, body_end int) {
	if !is_post {
		if path == '/feed' {
			s.feed()
		} else if path.starts_with('/posts/') {
			id := parse_id(path[7..])
			if id <= 0 {
				s.error(400, 'invalid post id')
			} else {
				s.post(id)
			}
		} else if path == '/health' {
			s.health()
		} else {
			s.error(404, 'not found')
		}
		return
	}
	if path == '/posts' || (path.starts_with('/posts/') && path.ends_with('/like')) {
		user_id, username := s.auth(buf, auth_start, auth_end) or {
			s.error(401, err.msg())
			return
		}
		if path == '/posts' {
			s.create(user_id, username, unsafe { tos(&buf[0] + body_start, body_end - body_start) })
		} else {
			id := parse_id(path[7..path.len - 5])
			if id <= 0 {
				s.error(400, 'invalid post id')
			} else {
				s.like(user_id, id)
			}
		}
		return
	}
	s.error(404, 'not found')
}

// parse_id returns the positive integer in s, or 0 when s is not one.
fn parse_id(s string) i64 {
	if s.len == 0 || s.len > 18 {
		return 0
	}
	mut v := i64(0)
	for c in s {
		if c < `0` || c > `9` {
			return 0
		}
		v = v * 10 + i64(c - `0`)
	}
	return v
}

// auth verifies the HS256 bearer token and returns (user id, username).
fn (mut s Server) auth(buf []u8, start int, end int) !(i64, string) {
	if start < 0 || end - start < 7 || unsafe { tos(&buf[0] + start, 7) } != 'Bearer ' {
		return error('missing bearer token')
	}
	token := unsafe { tos(&buf[0] + start + 7, end - start - 7) }.trim_space()
	dot1 := token.index_u8(`.`)
	dot2 := token.last_index_u8(`.`)
	if dot1 <= 0 || dot2 <= dot1 || token.len - dot2 - 1 != 43
		|| token[dot1 + 1..dot2].contains('.') {
		return error('invalid or expired token')
	}
	mut mac := [32]u8{}
	mut mac_len := u32(0)
	C.HMAC(C.EVP_sha256(), s.secret.data, s.secret.len, token.str, dot2, &mac[0], &mac_len)
	mut sig := [43]u8{}
	b64url_encode_32(mac, mut sig)
	mut diff := u8(0)
	for k in 0 .. 43 {
		diff |= sig[k] ^ token[dot2 + 1 + k]
	}
	if diff != 0 {
		return error('invalid or expired token')
	}
	head := b64url_decode(token[..dot1]) or { return error('invalid or expired token') }
	alg := claim(head, 'alg') or { return error('invalid or expired token') }
	if !alg.is_str || alg.text != 'HS256' {
		return error('invalid or expired token')
	}
	payload := b64url_decode(token[dot1 + 1..dot2]) or { return error('invalid or expired token') }
	exp := claim(payload, 'exp') or { return error('invalid or expired token') }
	if exp.is_str || exp.text.len == 0 || exp.text[0] == `-` {
		return error('invalid or expired token')
	}
	if exp.text.f64() <= f64(time.now().unix()) {
		return error('invalid or expired token')
	}
	sub := claim(payload, 'sub') or { return error('invalid token payload') }
	username := claim(payload, 'username') or { return error('invalid token payload') }
	if !sub.is_str || !username.is_str {
		return error('invalid token payload')
	}
	user_id := parse_id(sub.text)
	if user_id <= 0 {
		return error('invalid token payload')
	}
	return user_id, username.text
}

struct Claim {
	text   string
	is_str bool
}

// claim returns the top-level member `key` of a JSON object: a string (without escapes) or the
// raw text of a number, literal, array or object. none when the object is malformed or has no such key.
fn claim(src string, key string) ?Claim {
	mut i := skip_ws(src, 0)
	if i >= src.len || src[i] != `{` {
		return none
	}
	i = skip_ws(src, i + 1)
	if i < src.len && src[i] == `}` {
		return none
	}
	for i < src.len {
		if src[i] != `"` {
			return none
		}
		ks := i + 1
		i = json_string_end(src, ks) or { return none }
		k := src[ks..i]
		i = skip_ws(src, i + 1)
		if i >= src.len || src[i] != `:` {
			return none
		}
		i = skip_ws(src, i + 1)
		vs := i
		is_str := i < src.len && src[i] == `"`
		if is_str {
			i = json_string_end(src, i + 1) or { return none }
			if k == key {
				v := src[vs + 1..i]
				if v.contains('\\') {
					return none
				}
				return Claim{v, true}
			}
			i++
		} else {
			i = json_value_end(src, i) or { return none }
			if k == key {
				return Claim{src[vs..i], false}
			}
		}
		i = skip_ws(src, i)
		if i >= src.len || src[i] != `,` {
			return none
		}
		i = skip_ws(src, i + 1)
	}
	return none
}

fn skip_ws(src string, from int) int {
	mut i := from
	for i < src.len && (src[i] == ` ` || src[i] == `\n` || src[i] == `\r` || src[i] == `\t`) {
		i++
	}
	return i
}

// json_string_end returns the index of the closing quote of the string that starts after `from`.
fn json_string_end(src string, from int) ?int {
	mut i := from
	for i < src.len {
		if src[i] == `"` {
			return i
		}
		if src[i] == `\\` {
			i++
		}
		i++
	}
	return none
}

// json_value_end returns the index after a number, literal, array or object that starts at `from`.
fn json_value_end(src string, from int) ?int {
	mut i := from
	mut depth := 0
	for i < src.len {
		c := src[i]
		if c == `"` {
			i = json_string_end(src, i + 1) or { return none }
		} else if c == `{` || c == `[` {
			depth++
		} else if c == `}` || c == `]` {
			if depth == 0 {
				break
			}
			depth--
		} else if depth == 0 && (c == `,` || c == ` ` || c == `\n` || c == `\r` || c == `\t`) {
			break
		}
		i++
	}
	if i == from || depth != 0 || i >= src.len {
		return none
	}
	return i
}

// body_member decodes the "body" member of a post's JSON document (RFC 8259, at most 64 levels,
// raw control characters allowed in strings as x.json2 allows them). It returns none when raw is
// not JSON, and '' when the document has no string "body"; a repeated key counts the last time,
// as in a JSON object decoded to a map. The result points into body_buf.
// It replaces x.json2, whose decode never returns on input such as `[0`.
fn body_member(raw string, mut key_buf []u8, mut body_buf []u8) ?string {
	mut i := skip_ws(raw, 0)
	mut found := false
	if i < raw.len && raw[i] == `{` {
		i = skip_ws(raw, i + 1)
		if i < raw.len && raw[i] == `}` {
			i++
		} else {
			for {
				key_buf.clear()
				i = json_string(raw, i, mut key_buf, true)?
				i = skip_ws(raw, i)
				if i >= raw.len || raw[i] != `:` {
					return none
				}
				i = skip_ws(raw, i + 1)
				is_body := key_buf.len == 4 && unsafe { tos(key_buf.data, 4) } == 'body'
				if is_body && i < raw.len && raw[i] == `"` {
					body_buf.clear()
					i = json_string(raw, i, mut body_buf, true)?
					found = true
				} else {
					found = found && !is_body
					i = json_value(raw, i, 1, mut key_buf)?
				}
				i = skip_ws(raw, i)
				if i < raw.len && raw[i] == `,` {
					i = skip_ws(raw, i + 1)
				} else if i < raw.len && raw[i] == `}` {
					i++
					break
				} else {
					return none
				}
			}
		}
	} else {
		i = json_value(raw, i, 0, mut key_buf)?
	}
	if skip_ws(raw, i) != raw.len {
		return none
	}
	return if found { unsafe { tos(body_buf.data, body_buf.len) } } else { '' }
}

// json_value checks the JSON value at raw[from] and returns the index after it.
fn json_value(raw string, from int, depth int, mut scratch []u8) ?int {
	if from >= raw.len || depth > 64 {
		return none
	}
	c := raw[from]
	if c == `"` {
		return json_string(raw, from, mut scratch, false)
	}
	if c == `t` {
		return json_literal(raw, from, 'true')
	}
	if c == `f` {
		return json_literal(raw, from, 'false')
	}
	if c == `n` {
		return json_literal(raw, from, 'null')
	}
	if c != `{` && c != `[` {
		return json_number(raw, from)
	}
	close := if c == `{` { u8(`}`) } else { u8(`]`) }
	mut i := skip_ws(raw, from + 1)
	if i < raw.len && raw[i] == close {
		return i + 1
	}
	for i < raw.len {
		if c == `{` {
			i = json_string(raw, i, mut scratch, false)?
			i = skip_ws(raw, i)
			if i >= raw.len || raw[i] != `:` {
				return none
			}
			i = skip_ws(raw, i + 1)
		}
		i = json_value(raw, i, depth + 1, mut scratch)?
		i = skip_ws(raw, i)
		if i < raw.len && raw[i] == close {
			return i + 1
		}
		if i >= raw.len || raw[i] != `,` {
			return none
		}
		i = skip_ws(raw, i + 1)
	}
	return none
}

fn json_literal(raw string, from int, lit string) ?int {
	if from + lit.len > raw.len || unsafe { raw.substr_unsafe(from, from + lit.len) } != lit {
		return none
	}
	return from + lit.len
}

// json_number checks -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? at raw[from] and returns the index after it.
fn json_number(raw string, from int) ?int {
	mut i := from
	if i < raw.len && raw[i] == `-` {
		i++
	}
	if i < raw.len && raw[i] == `0` {
		i++
	} else {
		i = json_digits(raw, i)?
	}
	if i < raw.len && raw[i] == `.` {
		i = json_digits(raw, i + 1)?
	}
	if i < raw.len && (raw[i] == `e` || raw[i] == `E`) {
		i++
		if i < raw.len && (raw[i] == `+` || raw[i] == `-`) {
			i++
		}
		i = json_digits(raw, i)?
	}
	return i
}

fn json_digits(raw string, from int) ?int {
	mut i := from
	for i < raw.len && raw[i] >= `0` && raw[i] <= `9` {
		i++
	}
	if i == from {
		return none
	}
	return i
}

// json_string checks the JSON string at raw[from] and returns the index after its closing quote.
// With decode it appends the decoded bytes to out.
fn json_string(raw string, from int, mut out []u8, decode bool) ?int {
	if from >= raw.len || raw[from] != `"` {
		return none
	}
	mut i := from + 1
	mut run := i
	for i < raw.len {
		c := raw[i]
		if c != `"` && c != `\\` {
			i++
			continue
		}
		if decode && i > run {
			unsafe { out.push_many(raw.str + run, i - run) }
		}
		if c == `"` {
			return i + 1
		}
		if i + 1 >= raw.len {
			return none
		}
		e := raw[i + 1]
		i += 2
		if e == `u` {
			mut cp := json_hex4(raw, i)?
			i += 4
			if cp >= 0xdc00 && cp <= 0xdfff {
				return none
			}
			if cp >= 0xd800 && cp <= 0xdbff {
				if i + 1 >= raw.len || raw[i] != `\\` || raw[i + 1] != `u` {
					return none
				}
				lo := json_hex4(raw, i + 2)?
				if lo < 0xdc00 || lo > 0xdfff {
					return none
				}
				i += 6
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00)
			}
			if decode {
				put_utf8(mut out, cp)
			}
		} else {
			b := match e {
				`"`, `\\`, `/` { e }
				`b` { u8(8) }
				`f` { u8(12) }
				`n` { u8(10) }
				`r` { u8(13) }
				`t` { u8(9) }
				else { return none }
			}
			if decode {
				out << b
			}
		}
		run = i
	}
	return none
}

fn json_hex4(raw string, from int) ?u32 {
	if from + 4 > raw.len {
		return none
	}
	mut v := u32(0)
	for k in from .. from + 4 {
		c := raw[k]
		mut d := u32(0)
		if c >= `0` && c <= `9` {
			d = u32(c - `0`)
		} else if c >= `a` && c <= `f` {
			d = u32(c - `a` + 10)
		} else if c >= `A` && c <= `F` {
			d = u32(c - `A` + 10)
		} else {
			return none
		}
		v = v << 4 | d
	}
	return v
}

fn put_utf8(mut out []u8, cp u32) {
	if cp < 0x80 {
		out << u8(cp)
	} else if cp < 0x800 {
		out << u8(0xc0 | cp >> 6)
		out << u8(0x80 | cp & 0x3f)
	} else if cp < 0x10000 {
		out << u8(0xe0 | cp >> 12)
		out << u8(0x80 | (cp >> 6) & 0x3f)
		out << u8(0x80 | cp & 0x3f)
	} else {
		out << u8(0xf0 | cp >> 18)
		out << u8(0x80 | (cp >> 12) & 0x3f)
		out << u8(0x80 | (cp >> 6) & 0x3f)
		out << u8(0x80 | cp & 0x3f)
	}
}

fn (mut s Server) create(user_id i64, username string, raw string) {
	body := body_member(raw, mut s.jkey, mut s.jbody) or {
		s.error(400, 'malformed JSON body')
		return
	}
	text := body.trim_space()
	if text.len == 0 {
		s.error(400, 'body is required')
		return
	}
	if text.len > 500 && text.len_utf8() > 500 {
		s.error(400, 'body must be at most 500 characters')
		return
	}
	s.txn()
	st := s.st_insert
	C.sqlite3_bind_int64(st, 1, user_id)
	C.sqlite3_bind_text(st, 2, &char(text.str), text.len, unsafe { nil })
	if C.sqlite3_step(st) != sqlite_row {
		C.sqlite3_reset(st)
		s.error(500, 'internal server error')
		return
	}
	s.start_body()
	put(mut s.out, '{"post":{"id":')
	put_int(mut s.out, C.sqlite3_column_int64(st, 0))
	put(mut s.out, ',"body":')
	put_json(mut s.out, text.str, text.len)
	put(mut s.out, ',"created_at":')
	put_json(mut s.out, C.sqlite3_column_text(st, 1), C.sqlite3_column_bytes(st, 1))
	put(mut s.out, ',"author":')
	put_json(mut s.out, username.str, username.len)
	put(mut s.out, ',"like_count":0}}')
	C.sqlite3_reset(st)
	s.respond(201)
}

fn (mut s Server) like(user_id i64, post_id i64) {
	s.txn()
	C.sqlite3_bind_int64(s.st_exists, 1, post_id)
	found := C.sqlite3_step(s.st_exists)
	C.sqlite3_reset(s.st_exists)
	if found == sqlite_done {
		s.error(404, 'post not found')
		return
	}
	C.sqlite3_bind_int64(s.st_like, 1, user_id)
	C.sqlite3_bind_int64(s.st_like, 2, post_id)
	rc := C.sqlite3_step(s.st_like)
	C.sqlite3_reset(s.st_like)
	if found != sqlite_row || rc != sqlite_done {
		s.error(500, 'internal server error')
		return
	}
	created := C.sqlite3_changes(s.db) == 1
	s.start_body()
	put(mut s.out, if created {
		'{"liked":true,"already_liked":false,"post_id":'
	} else {
		'{"liked":true,"already_liked":true,"post_id":'
	})
	put_int(mut s.out, post_id)
	put_byte(mut s.out, `}`)
	s.respond(if created { 201 } else { 200 })
}

// feed reads the newest 20 ids, then fetches their rows and like counts with one
// range scan each when the ids are close together (they are, as ids grow with time).
fn (mut s Server) feed() {
	s.txn()
	mut ids := [20]i64{}
	mut n := 0
	mut rc := C.sqlite3_step(s.st_top)
	for rc == sqlite_row && n < 20 {
		ids[n] = C.sqlite3_column_int64(s.st_top, 0)
		n++
		rc = C.sqlite3_step(s.st_top)
	}
	C.sqlite3_reset(s.st_top)
	mut lo := ids[0]
	mut hi := ids[0]
	for k in 1 .. n {
		lo = if ids[k] < lo { ids[k] } else { lo }
		hi = if ids[k] > hi { ids[k] } else { hi }
	}
	if rc == sqlite_done && n > 0 && hi - lo < feed_span && s.feed_range(ids, n, lo, hi) {
		return
	}
	s.feed_full()
}

// feed_range scans the rows newest-id first, which is feed order when ids fall with created_at,
// so each row goes straight into the response; any other order returns false for feed_full.
fn (mut s Server) feed_range(ids [20]i64, n int, lo i64, hi i64) bool {
	mut likes := [feed_span]i64{}
	C.sqlite3_bind_int64(s.st_counts, 1, lo)
	C.sqlite3_bind_int64(s.st_counts, 2, hi)
	mut rc := C.sqlite3_step(s.st_counts)
	for rc == sqlite_row {
		likes[C.sqlite3_column_int64(s.st_counts, 0) - lo]++
		rc = C.sqlite3_step(s.st_counts)
	}
	C.sqlite3_reset(s.st_counts)
	if rc != sqlite_done {
		return false
	}
	s.start_body()
	put(mut s.out, '{"posts":[')
	C.sqlite3_bind_int64(s.st_range, 1, lo)
	C.sqlite3_bind_int64(s.st_range, 2, hi)
	mut k := 0
	rc = C.sqlite3_step(s.st_range)
	for rc == sqlite_row && k < n {
		id := C.sqlite3_column_int64(s.st_range, 0)
		if id < ids[k] {
			break
		}
		if id == ids[k] {
			if k > 0 {
				put(mut s.out, ',')
			}
			put_post_head(mut s.out, s.st_range, id)
			put(mut s.out, ',"like_count":')
			put_int(mut s.out, likes[id - lo])
			put(mut s.out, '}')
			k++
		}
		rc = C.sqlite3_step(s.st_range)
	}
	C.sqlite3_reset(s.st_range)
	if k < n {
		return false
	}
	put(mut s.out, ']}')
	s.respond(200)
	return true
}

// feed_full is the plain feed query, for ids too far apart for feed_range.
fn (mut s Server) feed_full() {
	st := s.st_feed
	s.start_body()
	put(mut s.out, '{"posts":[')
	mut rc := C.sqlite3_step(st)
	for rc == sqlite_row {
		if s.out.len - s.body_start > 10 {
			put_byte(mut s.out, `,`)
		}
		s.put_post(st)
		rc = C.sqlite3_step(st)
	}
	C.sqlite3_reset(st)
	if rc != sqlite_done {
		s.error(500, 'internal server error')
		return
	}
	put(mut s.out, ']}')
	s.respond(200)
}

fn (mut s Server) post(id i64) {
	s.txn()
	st := s.st_post
	C.sqlite3_bind_int64(st, 1, id)
	rc := C.sqlite3_step(st)
	if rc == sqlite_row {
		s.start_body()
		put(mut s.out, '{"post":')
		s.put_post(st)
		put_byte(mut s.out, `}`)
		C.sqlite3_reset(st)
		s.respond(200)
		return
	}
	C.sqlite3_reset(st)
	if rc == sqlite_done {
		s.error(404, 'post not found')
	} else {
		s.error(500, 'internal server error')
	}
}

fn (mut s Server) put_post(st &C.sqlite3_stmt) {
	put_post_head(mut s.out, st, C.sqlite3_column_int64(st, 0))
	put(mut s.out, ',"like_count":')
	put_int(mut s.out, C.sqlite3_column_int64(st, 4))
	put(mut s.out, '}')
}

// put_post_head writes a post object up to (not including) its like_count.
fn put_post_head(mut b []u8, st &C.sqlite3_stmt, id i64) {
	put(mut b, '{"id":')
	put_int(mut b, id)
	put(mut b, ',"body":')
	put_json(mut b, C.sqlite3_column_text(st, 1), C.sqlite3_column_bytes(st, 1))
	put(mut b, ',"created_at":')
	// shortcut: created_at is always the schema's 24-byte UTC wire format with nothing to escape; revisit if that format changes
	append(mut b, c'"', 1)
	append(mut b, C.sqlite3_column_text(st, 2), 24)
	append(mut b, c'"', 1)
	put(mut b, ',"author":')
	put_json(mut b, C.sqlite3_column_text(st, 3), C.sqlite3_column_bytes(st, 3))
}

fn (mut s Server) health() {
	s.txn()
	rc := C.sqlite3_step(s.st_health)
	C.sqlite3_reset(s.st_health)
	s.start_body()
	if rc != sqlite_row {
		put(mut s.out, '{"status":"degraded","db":"unreachable","error":')
		msg := unsafe { cstring_to_vstring(C.sqlite3_errmsg(s.db)) }
		put_json(mut s.out, msg.str, msg.len)
		put_byte(mut s.out, `}`)
		s.respond(503)
		return
	}
	put(mut s.out, '{"status":"ok","db":"ok","uptime_s":')
	put_int(mut s.out, time.now().unix() - s.started)
	put_byte(mut s.out, `}`)
	s.respond(200)
}

fn (mut s Server) error(status int, msg string) {
	s.start_body()
	put(mut s.out, '{"error":"')
	put(mut s.out, msg)
	put(mut s.out, '"}')
	s.respond(status)
}

// start_body opens a response body in out after room for its header. Called again before
// respond, it drops what the open body has so far.
fn (mut s Server) start_body() {
	if s.body_start >= 0 {
		unsafe {
			s.out.len = s.body_start
		}
		return
	}
	if s.out.len + header_room > s.out.cap {
		s.out.ensure_cap(s.out.len + header_room)
	}
	unsafe {
		s.out.len += header_room
	}
	s.body_start = s.out.len
}

// respond puts the header right before the open body, so the body is never copied, and queues
// the response as a segment for after_parse to assign to its connection.
fn (mut s Server) respond(status int) {
	s.txn()
	s.head.clear()
	put(mut s.head, match status {
		200 { 'HTTP/1.1 200 OK\r\n' }
		201 { 'HTTP/1.1 201 Created\r\n' }
		400 { 'HTTP/1.1 400 Bad Request\r\n' }
		401 { 'HTTP/1.1 401 Unauthorized\r\n' }
		404 { 'HTTP/1.1 404 Not Found\r\n' }
		503 { 'HTTP/1.1 503 Service Unavailable\r\n' }
		else { 'HTTP/1.1 500 Internal Server Error\r\n' }
	})
	if s.cur_close {
		put(mut s.head, 'Connection: close\r\n')
	}
	put(mut s.head, 'Content-Type: application/json\r\nContent-Length: ')
	put_int(mut s.head, s.out.len - s.body_start)
	put(mut s.head, '\r\n\r\n')
	at := s.body_start - s.head.len
	unsafe { vmemcpy(&u8(s.out.data) + at, s.head.data, s.head.len) }
	s.segs << Seg{-1, at, s.out.len}
	s.body_start = -1
}

// append copies n bytes to the end of b; it skips push_many's per-call checks on the hot path.
@[inline]
fn append(mut b []u8, p &u8, n int) {
	if b.len + n > b.cap {
		b.ensure_cap(b.len + n)
	}
	unsafe {
		vmemcpy(&u8(b.data) + b.len, p, n)
		b.len += n
	}
}

@[inline]
fn put(mut b []u8, s string) {
	n := s.len
	if b.len + n > b.cap {
		b.ensure_cap(b.len + n)
	}
	unsafe {
		d := &u8(b.data) + b.len
		if n >= 8 && n <= 16 { // two overlapping 8-byte moves instead of a memcpy call
			a := *(&u64(voidptr(s.str)))
			z := *(&u64(voidptr(s.str + n - 8)))
			*(&u64(voidptr(d))) = a
			*(&u64(voidptr(d + n - 8))) = z
		} else {
			vmemcpy(d, s.str, n)
		}
		b.len += n
	}
}

@[inline]
fn put_byte(mut b []u8, c u8) {
	if b.len + 1 > b.cap {
		b.ensure_cap(b.len + 1)
	}
	unsafe {
		*(&u8(b.data) + b.len) = c
		b.len++
	}
}

const digit_pairs = '00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899'

fn put_int(mut b []u8, v i64) {
	if b.len + 21 > b.cap {
		b.ensure_cap(b.len + 21)
	}
	mut n := u64(v)
	if v < 0 {
		n = u64(0) - n
	}
	mut digits := 1
	mut limit := u64(10)
	for n >= limit {
		digits++
		limit *= 10
	}
	neg := if v < 0 { 1 } else { 0 }
	unsafe {
		mut e := &u8(b.data) + b.len + neg + digits
		for n >= 100 {
			r := int(n % 100) * 2
			n /= 100
			e -= 2
			e[0] = digit_pairs.str[r]
			e[1] = digit_pairs.str[r + 1]
		}
		if n >= 10 {
			r := int(n) * 2
			e -= 2
			e[0] = digit_pairs.str[r]
			e[1] = digit_pairs.str[r + 1]
		} else {
			e--
			e[0] = u8(`0` + int(n))
		}
		if neg == 1 {
			*(&u8(b.data) + b.len) = `-`
		}
		b.len += neg + digits
	}
}

const b64url = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'

// b64url_encode_32 writes the 43-character unpadded base64url form of a 32-byte MAC.
fn b64url_encode_32(src [32]u8, mut dst [43]u8) {
	mut o := 0
	for i := 0; i < 30; i += 3 {
		v := u32(src[i]) << 16 | u32(src[i + 1]) << 8 | u32(src[i + 2])
		dst[o] = b64url[v >> 18]
		dst[o + 1] = b64url[(v >> 12) & 63]
		dst[o + 2] = b64url[(v >> 6) & 63]
		dst[o + 3] = b64url[v & 63]
		o += 4
	}
	v := u32(src[30]) << 16 | u32(src[31]) << 8
	dst[40] = b64url[v >> 18]
	dst[41] = b64url[(v >> 12) & 63]
	dst[42] = b64url[(v >> 6) & 63]
}

const b64url_rev = b64url_table()

// b64url_table maps each byte to its base64url value, or 255 when it is not a base64url digit.
fn b64url_table() [256]u8 {
	mut t := [256]u8{init: 255}
	for i in 0 .. 64 {
		t[b64url[i]] = u8(i)
	}
	return t
}

// b64url_decode decodes unpadded base64url, or returns none on a bad character or length.
fn b64url_decode(s string) ?string {
	if s.len % 4 == 1 {
		return none
	}
	n := s.len * 3 / 4
	mut out := unsafe { malloc_noscan(n + 1) }
	mut acc := u32(0)
	mut bits := 0
	mut o := 0
	for c in s {
		v := b64url_rev[c]
		if v == 255 {
			return none
		}
		acc = acc << 6 | u32(v)
		bits += 6
		if bits >= 8 {
			bits -= 8
			unsafe {
				out[o] = u8(acc >> u32(bits))
			}
			o++
		}
	}
	unsafe {
		out[o] = 0
		return tos(out, o)
	}
}

const hex = '0123456789abcdef'

// put_json writes p[0..n] as a JSON string, escaped the way JSON.stringify does.
fn put_json(mut b []u8, p &u8, n int) {
	mut i := C.td_json_scan(p, n)
	if i == n {
		if b.len + n + 2 > b.cap {
			b.ensure_cap(b.len + n + 2)
		}
		unsafe {
			d := &u8(b.data) + b.len
			d[0] = `"`
			vmemcpy(d + 1, p, n)
			d[n + 1] = `"`
			b.len += n + 2
		}
		return
	}
	append(mut b, c'"', 1)
	mut from := 0
	for i < n {
		c := unsafe { p[i] }
		append(mut b, unsafe { p + from }, i - from)
		i++
		from = i
		b << `\\`
		match c {
			`"` {
				b << `"`
			}
			`\\` {
				b << `\\`
			}
			`\n` {
				b << `n`
			}
			`\r` {
				b << `r`
			}
			`\t` {
				b << `t`
			}
			0x08 {
				b << `b`
			}
			0x0c {
				b << `f`
			}
			else {
				put(mut b, 'u00')
				b << hex[c >> 4]
				b << hex[c & 15]
			}
		}
		i += C.td_json_scan(unsafe { p + i }, n - i)
	}
	append(mut b, unsafe { p + from }, n - from)
	append(mut b, c'"', 1)
}
