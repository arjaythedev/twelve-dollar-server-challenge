// A single-threaded epoll HTTP/1.1 server for the $12 challenge feed API.
// Every epoll_wait batch runs inside one SQLite transaction (group commit):
// responses are queued while the batch is processed and written only after COMMIT.
module main

import net
import os
import time
import x.json2

#flag -I @DIR/build
#flag @DIR/build/sqlite3.o
#flag -lm -lcrypto
#include <openssl/hmac.h>
#include "sqlite3.h"
#include <sys/epoll.h>
#include <sys/resource.h>
#include <netinet/tcp.h>
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
fn C.sqlite3_wal_checkpoint_v2(&C.sqlite3, &char, i32, &i32, &i32) i32
fn C.EVP_sha256() voidptr

// Only the fd member is declared: a pointer member makes V's GC scan every event in the array.
union C.epoll_data {
mut:
	fd i32
}

@[packed]
struct C.epoll_event {
mut:
	events u32
	data   C.epoll_data
}

struct C.rlimit {
mut:
	rlim_cur u64
	rlim_max u64
}

fn C.epoll_create1(i32) i32
fn C.epoll_ctl(i32, i32, i32, &C.epoll_event) i32
fn C.epoll_wait(i32, &C.epoll_event, i32, i32) i32
fn C.accept4(i32, voidptr, voidptr, i32) i32
fn C.setsockopt(i32, i32, i32, voidptr, u32) i32
fn C.getrlimit(i32, &C.rlimit) i32
fn C.setrlimit(i32, &C.rlimit) i32

const sqlite_row = 100
const sqlite_done = 101
const ep_in = u32(0x001)
const ep_out = u32(0x004)
const ep_err = u32(0x008)
const ep_hup = u32(0x010)
const ep_et = u32(0x80000000)
const max_events = 1024
const max_request = 65536

const sql_post = 'SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id'

const feed_span = 64 // feed_range handles the newest 20 ids when they span fewer ids than this
const ckpt_frames = 1000 // WAL size that starts a checkpoint (SQLite's default auto-checkpoint)
const ckpt_max_frames = 16000 // past this WAL size, phase B holds writes until the WAL is copied
const max_pause_ms = u64(20) // longest hold on write requests while the WAL can restart

struct Conn {
mut:
	inbuf       []u8 // bytes of a request that has not fully arrived
	pending     []u8 // response bytes the socket did not take yet
	close_after bool
	queued      bool // already in this batch's flush list
	open        bool
	deferred    bool // holds a write request until the running checkpoint finishes
	want_out    bool // registered for EPOLLOUT
	prev        int = -1 // neighbours in the least-recently-active list
	next        int = -1
}

struct Seg {
	fd    int
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
	max_open  int // descriptor budget for client connections
	n_open    int
	lru_head  int = -1 // least recently active connection
	lru_tail  int = -1
	conns     []Conn
	rbuf      []u8
	out       []u8 // all responses of the current batch
	body      []u8 // scratch for one response body
	rows      []u8 // scratch for feed rows before they are put in feed order
	segs      []Seg
	flush     []int
	cur_close bool
	// Cooperative checkpoint: see checkpointer.
	wal_frames    int
	ckpt_phase    int // 0 idle, 1 checkpoint with writes going on, 2 checkpoint with writes held
	writes_paused bool
	pause_until   u64 // monotonic ns when held writes go on anyway (0: wait for the checkpoint)
	ckpt_req      chan bool = chan bool{cap: 1}
	ckpt_efd      int
	epfd          int
	deferring     bool // set by parse when it stops at a write request
	deferred      []int
}

fn main() {
	mut s := &Server{
		secret:  os.getenv('JWT_SECRET').bytes()
		started: time.now().unix()
		rbuf:    []u8{len: max_request}
		out:     []u8{cap: 1 << 20}
		body:    []u8{cap: 16384}
		rows:    []u8{cap: 16384}
		conns:   []Conn{len: 1024}
	}
	path := os.getenv('SQLITE_PATH')
	s.open_db(path)
	s.ckpt_efd = C.eventfd(0, C.EFD_NONBLOCK | C.EFD_CLOEXEC)
	spawn checkpointer(path, s.ckpt_req, s.ckpt_efd)
	mut rl := C.rlimit{}
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	rl.rlim_cur = rl.rlim_max
	C.setrlimit(C.RLIMIT_NOFILE, &rl)
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	s.max_open = int(rl.rlim_cur) - 64 // leave descriptors for SQLite, epoll and the listener
	host := os.getenv_opt('HOST') or { '0.0.0.0' }
	port := os.getenv_opt('PORT') or { '3000' }
	mut l := net.listen_tcp(.ip, '${host}:${port}', backlog: 65535) or { panic(err) }
	s.run(int(l.sock.handle))
}

// checkpointer copies the WAL into the database file on its own connection and thread, so the
// fsyncs a checkpoint needs never block the event loop. Each request runs one PASSIVE checkpoint
// and signals the eventfd with 1 when it copied the whole WAL (the next write then restarts the
// WAL from its beginning) or 2 when frames were appended meanwhile.
fn checkpointer(path string, req chan bool, efd int) {
	mut db := &C.sqlite3(unsafe { nil })
	if C.sqlite3_open_v2(&char(path.str), &db, 2, unsafe { nil }) != 0 {
		panic('checkpointer: cannot open ${path}')
	}
	C.sqlite3_exec(db, c'PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0', unsafe { nil },
		unsafe { nil }, unsafe { nil })
	for {
		_ := <-req
		mut log := i32(0)
		mut done := i32(0)
		rc := C.sqlite3_wal_checkpoint_v2(db, unsafe { nil }, 0, &log, &done)
		result := u64(if rc == 0 && log == done { 1 } else { 2 })
		C.write(efd, &result, 8)
	}
}

// wal_hook runs inside each COMMIT and records the WAL size in frames.
fn wal_hook(arg voidptr, _ &C.sqlite3, _ &char, frames i32) i32 {
	mut s := unsafe { &Server(arg) }
	s.wal_frames = frames
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
		c'PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=1000; PRAGMA cache_size=-65536; PRAGMA mmap_size=1073741824; PRAGMA temp_store=MEMORY',
		unsafe { nil }, unsafe { nil }, unsafe { nil })
	// Replaces SQLite's inline auto-checkpoint: checkpoints run on the checkpointer thread.
	C.sqlite3_wal_hook(s.db, wal_hook, s)
	s.st_feed = s.prepare(sql_post + ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20')
	s.st_top = s.prepare('SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20')
	s.st_range =
		s.prepare('SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?')
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

// txn opens the batch transaction on the first statement of a batch.
fn (mut s Server) txn() {
	if !s.in_txn {
		s.in_txn = run_once(s.st_begin)
	}
}

fn run_once(st &C.sqlite3_stmt) bool {
	rc := C.sqlite3_step(st)
	C.sqlite3_reset(st)
	return rc == sqlite_done
}

fn (mut s Server) run(lfd int) {
	C.fcntl(lfd, C.F_SETFL, C.fcntl(lfd, C.F_GETFL, 0) | C.O_NONBLOCK)
	epfd := C.epoll_create1(0)
	s.epfd = epfd
	mut ev := C.epoll_event{
		events: ep_in
	}
	ev.data.fd = lfd
	C.epoll_ctl(epfd, C.EPOLL_CTL_ADD, lfd, &ev)
	ev.data.fd = s.ckpt_efd
	C.epoll_ctl(epfd, C.EPOLL_CTL_ADD, s.ckpt_efd, &ev)
	mut events := []C.epoll_event{len: max_events}
	for {
		n := C.epoll_wait(epfd, events.data, max_events, s.pause_timeout())
		if s.writes_paused && s.pause_until > 0 && time.sys_mono_now() >= s.pause_until {
			s.resume_writes() // the disk is slow: let writes go on, try the restart later
		}
		for i in 0 .. n {
			fd := unsafe { events[i].data.fd }
			if fd == lfd {
				s.accept_all(lfd, epfd)
				continue
			}
			if fd == s.ckpt_efd {
				s.checkpoint_done()
				continue
			}
			flags := events[i].events
			if flags & (ep_in | ep_err | ep_hup) != 0 {
				s.read_conn(fd)
			}
			if flags & ep_out != 0 && s.conns[fd].pending.len > 0 {
				s.queue(fd)
			}
		}
		s.finish_batch()
	}
}

fn (mut s Server) accept_all(lfd int, epfd int) {
	one := 1
	for {
		if s.n_open >= s.max_open && !s.evict_idle() {
			return
		}
		fd := C.accept4(lfd, unsafe { nil }, unsafe { nil }, C.SOCK_NONBLOCK | C.SOCK_CLOEXEC)
		if fd < 0 {
			if (C.errno == C.EMFILE || C.errno == C.ENFILE) && s.evict_idle() {
				continue
			}
			return
		}
		C.setsockopt(fd, C.IPPROTO_TCP, C.TCP_NODELAY, &one, 4)
		for fd >= s.conns.len {
			s.conns << Conn{}
		}
		s.conns[fd] = Conn{
			open: true
		}
		s.n_open++
		s.touch(fd)
		mut ev := C.epoll_event{
			events: ep_in | ep_et
		}
		ev.data.fd = fd
		C.epoll_ctl(epfd, C.EPOLL_CTL_ADD, fd, &ev)
	}
}

// touch moves fd to the most recently active end of the list.
fn (mut s Server) touch(fd int) {
	if s.lru_tail == fd {
		return
	}
	s.unlink(fd)
	s.conns[fd].prev = s.lru_tail
	if s.lru_tail >= 0 {
		s.conns[s.lru_tail].next = fd
	} else {
		s.lru_head = fd
	}
	s.lru_tail = fd
}

fn (mut s Server) unlink(fd int) {
	mut c := &s.conns[fd]
	if c.prev >= 0 {
		s.conns[c.prev].next = c.next
	} else if s.lru_head == fd {
		s.lru_head = c.next
	}
	if c.next >= 0 {
		s.conns[c.next].prev = c.prev
	} else if s.lru_tail == fd {
		s.lru_tail = c.prev
	}
	c.prev = -1
	c.next = -1
}

fn (mut s Server) close_conn(fd int) {
	s.unlink(fd)
	s.conns[fd] = Conn{}
	s.n_open--
	C.close(fd)
}

// evict_idle closes the least recently active connection that has no work in flight,
// so the descriptor budget is never a hard cap on users. The client reconnects on its next request.
fn (mut s Server) evict_idle() bool {
	mut fd := s.lru_head
	for _ in 0 .. 64 {
		if fd < 0 {
			return false
		}
		c := s.conns[fd]
		if !c.queued && c.pending.len == 0 && c.inbuf.len == 0 {
			s.close_conn(fd)
			return true
		}
		fd = c.next
	}
	return false
}

fn (mut s Server) queue(fd int) {
	if !s.conns[fd].queued {
		s.conns[fd].queued = true
		s.flush << fd
	}
}

// read_conn drains the socket (edge-triggered) and answers every complete request in it.
fn (mut s Server) read_conn(fd int) {
	start := s.out.len
	s.cur_close = s.conns[fd].close_after
	if !s.conns[fd].open {
		return
	}
	s.touch(fd)
	mut closed := false
	for {
		r := C.read(fd, s.rbuf.data, max_request)
		if r <= 0 {
			if r == 0 || C.errno != C.EAGAIN {
				closed = true
			}
			break
		}
		if s.conns[fd].inbuf.len == 0 {
			used := s.parse(s.rbuf, r)
			if used < r {
				s.conns[fd].inbuf = s.rbuf[used..r].clone()
			}
		} else {
			unsafe { s.conns[fd].inbuf.push_many(&s.rbuf[0], r) }
			buf := s.conns[fd].inbuf
			used := s.parse(buf, buf.len)
			s.conns[fd].inbuf = if used < buf.len { buf[used..].clone() } else { []u8{} }
		}
		if s.conns[fd].inbuf.len > max_request {
			closed = true
			break
		}
		if r < max_request {
			break // a short read means the socket is drained
		}
	}
	s.after_parse(fd, start, closed)
}

// after_parse queues what parse produced for fd: responses, a close, or a deferred write request.
fn (mut s Server) after_parse(fd int, start int, closed bool) {
	if s.deferring {
		s.deferring = false
		if !s.conns[fd].deferred {
			s.conns[fd].deferred = true
			s.deferred << fd
		}
	}
	if s.out.len > start {
		s.segs << Seg{fd, start, s.out.len}
	}
	if closed || s.cur_close {
		s.conns[fd].close_after = true
	}
	if s.out.len > start || closed || s.cur_close {
		s.queue(fd)
	}
}

fn (mut s Server) pause_timeout() int {
	if !s.writes_paused || s.pause_until == 0 {
		return -1
	}
	left := i64(s.pause_until) - i64(time.sys_mono_now())
	return if left <= 0 { 0 } else { int(left / 1_000_000) + 1 }
}

// The loop starts a checkpoint (phase A) when the WAL reaches ckpt_frames; writes go on. If that
// checkpoint could not copy the whole WAL, phase B holds write requests (reads go on) and runs one
// more checkpoint, which only copies the frames appended during phase A. Phase B ends when that
// checkpoint finishes or after max_pause_ms, whichever is first; past ckpt_max_frames it waits.
// checkpoint_done runs when the checkpointer signals the end of a checkpoint.
fn (mut s Server) checkpoint_done() {
	mut v := u64(0)
	C.read(s.ckpt_efd, &v, 8)
	if s.ckpt_phase == 1 && v != 1 {
		s.ckpt_phase = 2
		s.writes_paused = true
		s.pause_until = if s.wal_frames >= ckpt_max_frames {
			u64(0)
		} else {
			time.sys_mono_now() + max_pause_ms * 1_000_000
		}
		s.ckpt_req <- true
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
		if !c.open || !was_deferred || c.inbuf.len == 0 {
			continue
		}
		start := s.out.len
		s.cur_close = c.close_after
		buf := c.inbuf
		used := s.parse(buf, buf.len)
		s.conns[fd].inbuf = if used < buf.len { buf[used..].clone() } else { []u8{} }
		s.after_parse(fd, start, false)
	}
	s.deferred.clear()
}

// finish_batch commits the batch transaction, then writes every queued response.
fn (mut s Server) finish_batch() {
	if s.in_txn {
		s.in_txn = false
		if !run_once(s.st_commit) {
			run_once(s.st_back)
			for seg in s.segs {
				s.conns[seg.fd].close_after = true
			}
			s.segs.clear()
		}
	}
	if s.wal_frames >= ckpt_frames && s.ckpt_phase == 0 {
		s.ckpt_phase = 1
		s.ckpt_req <- true
	}
	for seg in s.segs {
		mut c := &s.conns[seg.fd]
		if c.pending.len > 0 {
			unsafe { c.pending.push_many(&s.out[seg.start], seg.end - seg.start) }
			continue
		}
		w := C.write(seg.fd, unsafe { &s.out[seg.start] }, seg.end - seg.start)
		if w < seg.end - seg.start {
			start := seg.start + if w > 0 { w } else { 0 }
			c.pending = s.out[start..seg.end].clone()
		}
	}
	for fd in s.flush {
		mut c := &s.conns[fd]
		c.queued = false
		if c.pending.len > 0 {
			w := C.write(fd, c.pending.data, c.pending.len)
			if w == c.pending.len {
				c.pending = []u8{}
			} else if w > 0 {
				c.pending = c.pending[w..].clone()
			} else if C.errno != C.EAGAIN {
				c.pending = []u8{}
				c.close_after = true
			}
		}
		if c.close_after && c.pending.len == 0 {
			s.close_conn(fd)
		} else if (c.pending.len > 0) != c.want_out {
			// Watch for writability only while output is pending: no wakeup per completed write.
			c.want_out = c.pending.len > 0
			mut ev := C.epoll_event{
				events: if c.want_out { ep_in | ep_out | ep_et } else { ep_in | ep_et }
			}
			ev.data.fd = fd
			C.epoll_ctl(s.epfd, C.EPOLL_CTL_MOD, fd, &ev)
		}
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
	for i := from; i + 1 < end; i++ {
		if buf[i] == `\r` && buf[i + 1] == `\n` {
			return i
		}
	}
	return -1
}

// parse answers every complete request in buf[0..n] and returns the bytes consumed.
fn (mut s Server) parse(buf []u8, n int) int {
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
			mut colon := h
			for colon < e && buf[colon] != `:` {
				colon++
			}
			mut v := colon + 1
			for v < e && buf[v] == ` ` {
				v++
			}
			name_len := colon - h
			if name_len == 14 && eq_ci(buf, h, 'content-length') {
				for j in v .. e {
					if buf[j] >= `0` && buf[j] <= `9` {
						clen = clen * 10 + int(buf[j] - `0`)
					}
				}
			} else if name_len == 13 && eq_ci(buf, h, 'authorization') {
				auth_start = v
				auth_end = e
			} else if name_len == 10 && eq_ci(buf, h, 'connection') {
				close = e - v == 5 && eq_ci(buf, v, 'close')
			}
			h = e + 2
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

fn (mut s Server) create(user_id i64, username string, raw string) {
	doc := json2.decode[json2.Any](raw) or {
		s.error(400, 'malformed JSON body')
		return
	}
	mut text := ''
	if doc is map[string]json2.Any {
		v := doc['body'] or { json2.Any(json2.null) }
		if v is string {
			text = v.trim_space()
		}
	}
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
	s.body.clear()
	put(mut s.body, '{"post":{"id":')
	put_int(mut s.body, C.sqlite3_column_int64(st, 0))
	put(mut s.body, ',"body":')
	put_json(mut s.body, text.str, text.len)
	put(mut s.body, ',"created_at":')
	put_json(mut s.body, C.sqlite3_column_text(st, 1), C.sqlite3_column_bytes(st, 1))
	put(mut s.body, ',"author":')
	put_json(mut s.body, username.str, username.len)
	put(mut s.body, ',"like_count":0}}')
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
	s.body.clear()
	put(mut s.body, if created {
		'{"liked":true,"already_liked":false,"post_id":'
	} else {
		'{"liked":true,"already_liked":true,"post_id":'
	})
	put_int(mut s.body, post_id)
	s.body << `}`
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

fn (mut s Server) feed_range(ids [20]i64, n int, lo i64, hi i64) bool {
	mut start := [feed_span]int{init: -1}
	mut end := [feed_span]int{}
	mut likes := [feed_span]i64{}
	s.rows.clear()
	C.sqlite3_bind_int64(s.st_range, 1, lo)
	C.sqlite3_bind_int64(s.st_range, 2, hi)
	mut rc := C.sqlite3_step(s.st_range)
	for rc == sqlite_row {
		off := C.sqlite3_column_int64(s.st_range, 0) - lo
		start[off] = s.rows.len
		put_post_head(mut s.rows, s.st_range)
		end[off] = s.rows.len
		rc = C.sqlite3_step(s.st_range)
	}
	C.sqlite3_reset(s.st_range)
	if rc != sqlite_done {
		return false
	}
	C.sqlite3_bind_int64(s.st_counts, 1, lo)
	C.sqlite3_bind_int64(s.st_counts, 2, hi)
	rc = C.sqlite3_step(s.st_counts)
	for rc == sqlite_row {
		likes[C.sqlite3_column_int64(s.st_counts, 0) - lo]++
		rc = C.sqlite3_step(s.st_counts)
	}
	C.sqlite3_reset(s.st_counts)
	if rc != sqlite_done {
		return false
	}
	s.body.clear()
	put(mut s.body, '{"posts":[')
	for k in 0 .. n {
		off := ids[k] - lo
		if start[off] < 0 {
			return false
		}
		if k > 0 {
			s.body << `,`
		}
		unsafe { s.body.push_many(&s.rows[start[off]], end[off] - start[off]) }
		put(mut s.body, ',"like_count":')
		put_int(mut s.body, likes[off])
		s.body << `}`
	}
	put(mut s.body, ']}')
	s.respond(200)
	return true
}

// feed_full is the plain feed query, for ids too far apart for feed_range.
fn (mut s Server) feed_full() {
	st := s.st_feed
	s.body.clear()
	put(mut s.body, '{"posts":[')
	mut rc := C.sqlite3_step(st)
	for rc == sqlite_row {
		if s.body.len > 10 {
			s.body << `,`
		}
		s.put_post(st)
		rc = C.sqlite3_step(st)
	}
	C.sqlite3_reset(st)
	if rc != sqlite_done {
		s.error(500, 'internal server error')
		return
	}
	put(mut s.body, ']}')
	s.respond(200)
}

fn (mut s Server) post(id i64) {
	s.txn()
	st := s.st_post
	C.sqlite3_bind_int64(st, 1, id)
	rc := C.sqlite3_step(st)
	if rc == sqlite_row {
		s.body.clear()
		put(mut s.body, '{"post":')
		s.put_post(st)
		s.body << `}`
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
	put_post_head(mut s.body, st)
	put(mut s.body, ',"like_count":')
	put_int(mut s.body, C.sqlite3_column_int64(st, 4))
	s.body << `}`
}

// put_post_head writes a post object up to (not including) its like_count.
fn put_post_head(mut b []u8, st &C.sqlite3_stmt) {
	put(mut b, '{"id":')
	put_int(mut b, C.sqlite3_column_int64(st, 0))
	put(mut b, ',"body":')
	put_json(mut b, C.sqlite3_column_text(st, 1), C.sqlite3_column_bytes(st, 1))
	put(mut b, ',"created_at":')
	put_json(mut b, C.sqlite3_column_text(st, 2), C.sqlite3_column_bytes(st, 2))
	put(mut b, ',"author":')
	put_json(mut b, C.sqlite3_column_text(st, 3), C.sqlite3_column_bytes(st, 3))
}

fn (mut s Server) health() {
	s.txn()
	rc := C.sqlite3_step(s.st_health)
	C.sqlite3_reset(s.st_health)
	s.body.clear()
	if rc != sqlite_row {
		put(mut s.body, '{"status":"degraded","db":"unreachable","error":')
		msg := unsafe { cstring_to_vstring(C.sqlite3_errmsg(s.db)) }
		put_json(mut s.body, msg.str, msg.len)
		s.body << `}`
		s.respond(503)
		return
	}
	put(mut s.body, '{"status":"ok","db":"ok","uptime_s":')
	put_int(mut s.body, time.now().unix() - s.started)
	s.body << `}`
	s.respond(200)
}

fn (mut s Server) error(status int, msg string) {
	s.body.clear()
	put(mut s.body, '{"error":"')
	put(mut s.body, msg)
	put(mut s.body, '"}')
	s.respond(status)
}

fn (mut s Server) respond(status int) {
	put(mut s.out, match status {
		200 { 'HTTP/1.1 200 OK\r\n' }
		201 { 'HTTP/1.1 201 Created\r\n' }
		400 { 'HTTP/1.1 400 Bad Request\r\n' }
		401 { 'HTTP/1.1 401 Unauthorized\r\n' }
		404 { 'HTTP/1.1 404 Not Found\r\n' }
		503 { 'HTTP/1.1 503 Service Unavailable\r\n' }
		else { 'HTTP/1.1 500 Internal Server Error\r\n' }
	})
	put(mut s.out, 'Content-Type: application/json\r\nContent-Length: ')
	put_int(mut s.out, s.body.len)
	put(mut s.out, '\r\n\r\n')
	append(mut s.out, s.body.data, s.body.len)
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
	append(mut b, s.str, s.len)
}

fn put_int(mut b []u8, v i64) {
	mut tmp := [21]u8{}
	mut n := if v < 0 { -v } else { v }
	mut i := 21
	for {
		i--
		tmp[i] = u8(`0` + n % 10)
		n /= 10
		if n == 0 {
			break
		}
	}
	if v < 0 {
		i--
		tmp[i] = `-`
	}
	append(mut b, &tmp[i], 21 - i)
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
const ones = u64(0x0101010101010101)
const highs = u64(0x8080808080808080)

// put_json writes p[0..n] as a JSON string, escaped the way JSON.stringify does.
fn put_json(mut b []u8, p &u8, n int) {
	append(mut b, c'"', 1)
	mut from := 0
	mut i := 0
	for i < n {
		if i + 8 <= n {
			// skip 8 bytes at once when none is a control character, `"` or `\`
			w := unsafe { *(&u64(voidptr(p + i))) }
			q := w ^ (ones * 0x22)
			bs := w ^ (ones * 0x5c)
			if ((w - ones * 0x20) | (q - ones) | (bs - ones)) & ~w & highs == 0 {
				i += 8
				continue
			}
		}
		c := unsafe { p[i] }
		i++
		if c >= 0x20 && c != `"` && c != `\\` {
			continue
		}
		append(mut b, unsafe { p + from }, i - 1 - from)
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
	}
	append(mut b, unsafe { p + from }, n - from)
	append(mut b, c'"', 1)
}
