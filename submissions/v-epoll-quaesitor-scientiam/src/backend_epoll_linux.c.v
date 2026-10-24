module main

import fasthttp
import net
import os
import time

// The scored backend: one thread, one epoll set, one SQLite connection. Every epoll_wait
// batch is served, committed once, and only then flushed, so a 201 always means the row is
// on disk. Idle connections cost one 64-byte record and nothing else.

#flag linux -D_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// Only the fd member is declared: with a pointer member V's GC would scan the whole event
// array on every collection.
union C.epoll_data {
mut:
	fd int
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

fn C.epoll_create1(int) int
fn C.epoll_ctl(int, int, int, &C.epoll_event) int
fn C.epoll_wait(int, &C.epoll_event, int, int) int
fn C.accept4(int, voidptr, voidptr, int) int
fn C.socket(int, int, int) int
fn C.bind(int, voidptr, u32) int
fn C.listen(int, int) int
fn C.setsockopt(int, int, int, voidptr, u32) int
fn C.recv(int, voidptr, usize, int) int
fn C.send(int, voidptr, usize, int) int
fn C.close(int) int
fn C.getrlimit(int, &C.rlimit) int
fn C.setrlimit(int, &C.rlimit) int
fn C.fcntl(int, int, ...int) int

const ev_in = u32(C.EPOLLIN) | u32(C.EPOLLRDHUP)
const ev_out = u32(C.EPOLLOUT)
const ev_err = u32(C.EPOLLERR) | u32(C.EPOLLHUP)
const max_events = 256
const accept_burst = 64
const scratch_size = 64 * 1024
const max_requests_per_pass = 64
const listen_backlog = 4096
const stall_timeout_s = u32(120)
const sweep_every_s = u32(10)
const pool_max = 256
const pool_buf_max = u32(16 * 1024)

// Buf is a malloc'd byte range owned by one connection; ptr is nil while idle.
struct Buf {
mut:
	ptr &u8 = unsafe { nil }
	len u32
	cap u32
}

enum CState as u32 {
	free
	open    // reading requests
	parked  // a response did not fit the socket buffer: waiting for EPOLLOUT, not reading
	closing // flush what is pending, then close
}

// Conn is the whole per-connection state: 64 bytes, one cache line.
struct Conn {
mut:
	input      Buf // bytes of a request that has not fully arrived
	output     Buf // response bytes the socket has not taken yet
	flight     Buf // reserved for the io_uring backend (buffer handed to the kernel)
	sent       u32 // bytes of output already written
	generation u32
	touched    u32 // seconds since start when the connection last made progress
	state      CState
}

// Seg is one response in the batch arena, owned by connection fd.
struct Seg {
	fd    int
	start int
	end   int
	wrote bool
}

struct Loop {
mut:
	app        &App = unsafe { nil }
	epfd       int
	lfd        int
	conns      []Conn
	n_open     int
	max_open   int
	accepting  bool
	scratch    []u8
	out        []u8 // every response of the current batch
	segs       []Seg
	pool       []Buf
	start_ns   i64
	last_sweep u32
}

fn (l &Loop) seconds() u32 {
	return u32((time.sys_mono_now() - l.start_ns) / time.second)
}

// buf_take returns a buffer of at least need bytes, from the pool when one fits.
fn (mut l Loop) buf_take(need u32) Buf {
	for i := l.pool.len - 1; i >= 0; i-- {
		if l.pool[i].cap >= need {
			b := l.pool[i]
			l.pool.delete_last()
			return Buf{
				ptr: b.ptr
				cap: b.cap
			}
		}
	}
	mut cap := u32(4096)
	for cap < need {
		cap *= 2
	}
	return Buf{
		ptr: unsafe { &u8(C.malloc(cap)) }
		cap: cap
	}
}

// buf_give returns a buffer to the pool (or frees it when the pool is full or it is large).
fn (mut l Loop) buf_give(b Buf) {
	if b.ptr == unsafe { nil } {
		return
	}
	if l.pool.len < pool_max && b.cap <= pool_buf_max {
		l.pool << Buf{
			ptr: b.ptr
			cap: b.cap
		}
		return
	}
	unsafe { C.free(b.ptr) }
}

// buf_append grows b as needed and appends n bytes from p.
fn (mut l Loop) buf_append(mut b Buf, p &u8, n int) {
	need := b.len + u32(n)
	if b.ptr == unsafe { nil } {
		nb := l.buf_take(need)
		b.ptr = nb.ptr
		b.cap = nb.cap
		b.len = 0
	} else if need > b.cap {
		mut cap := b.cap
		for cap < need {
			cap *= 2
		}
		b.ptr = unsafe { &u8(C.realloc(b.ptr, cap)) }
		b.cap = cap
	}
	unsafe { vmemcpy(b.ptr + b.len, p, n) }
	b.len = need
}

fn (mut l Loop) buf_drop(mut b Buf) {
	l.buf_give(b)
	b.ptr = unsafe { nil }
	b.len = 0
	b.cap = 0
}

fn set_nonblocking(fd int) {
	flags := C.fcntl(fd, C.F_GETFL, 0)
	C.fcntl(fd, C.F_SETFL, flags | C.O_NONBLOCK)
}

fn epoll_add(epfd int, fd int, events u32) int {
	mut ev := C.epoll_event{
		events: events
	}
	ev.data.fd = fd
	return C.epoll_ctl(epfd, C.EPOLL_CTL_ADD, fd, &ev)
}

fn epoll_mod(epfd int, fd int, events u32) {
	mut ev := C.epoll_event{
		events: events
	}
	ev.data.fd = fd
	C.epoll_ctl(epfd, C.EPOLL_CTL_MOD, fd, &ev)
}

fn create_listener(host string, port int) !int {
	addrs := net.resolve_addrs('${host}:${port}', .ip, .tcp)!
	if addrs.len == 0 {
		return error('cannot resolve ${host}:${port}')
	}
	addr := addrs[0]
	fd := C.socket(int(addr.family()), int(net.SocketType.tcp), 0)
	if fd < 0 {
		return error('socket: errno ${C.errno}')
	}
	opt := 1
	C.setsockopt(fd, C.SOL_SOCKET, C.SO_REUSEADDR, &opt, sizeof(int))
	if os.getenv('SERVER_PROCS').int() > 1 {
		// several processes share one port; the kernel balances accepts between them
		C.setsockopt(fd, C.SOL_SOCKET, C.SO_REUSEPORT, &opt, sizeof(int))
	}
	if C.bind(fd, voidptr(&addr), addr.len()) < 0 {
		return error('bind ${host}:${port}: errno ${C.errno}')
	}
	if C.listen(fd, listen_backlog) < 0 {
		return error('listen: errno ${C.errno}')
	}
	set_nonblocking(fd)
	return fd
}

// run_epoll serves HOST:PORT on this thread until the process is stopped.
fn run_epoll(host string, port int) ! {
	procs := os.getenv('SERVER_PROCS').int()
	mut app := new_app(true, procs <= 1)!
	mut rl := C.rlimit{}
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	rl.rlim_cur = rl.rlim_max
	C.setrlimit(C.RLIMIT_NOFILE, &rl)
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	lfd := create_listener(host, port)!
	mut l := &Loop{
		app:       app
		epfd:      C.epoll_create1(C.EPOLL_CLOEXEC)
		lfd:       lfd
		conns:     []Conn{len: 4096}
		max_open:  int(rl.rlim_cur) - 64 // SQLite, the WAL, epoll and the listener need some
		accepting: true
		scratch:   []u8{len: scratch_size}
		out:       []u8{cap: 1 << 20}
		segs:      []Seg{cap: 1024}
		pool:      []Buf{cap: pool_max}
		start_ns:  time.sys_mono_now()
	}
	if l.epfd < 0 {
		return error('epoll_create1: errno ${C.errno}')
	}
	if epoll_add(l.epfd, lfd, u32(C.EPOLLIN)) < 0 {
		return error('epoll_ctl(listener): errno ${C.errno}')
	}
	assert sizeof(Conn) == 64
	println('listening on http://${host}:${port}/ (epoll backend, pid ${os.getpid()}, fd budget ${l.max_open})')
	flush_stdout()
	l.run()
}

fn (mut l Loop) run() {
	mut events := [max_events]C.epoll_event{}
	for {
		n := C.epoll_wait(l.epfd, &events[0], max_events, 1000)
		if n < 0 {
			if C.errno == C.EINTR {
				continue
			}
			eprintln('epoll_wait: errno ${C.errno}')
			continue
		}
		l.app.tick()
		now := l.seconds()
		for i in 0 .. n {
			fd := unsafe { events[i].data.fd }
			flags := unsafe { events[i].events }
			if fd == l.lfd {
				l.accept_burst()
				continue
			}
			if fd >= l.conns.len || l.conns[fd].state == .free {
				continue
			}
			if flags & ev_err != 0 && flags & ev_in == 0 {
				l.close_conn(fd)
				continue
			}
			if flags & ev_out != 0 && (l.conns[fd].state == .parked || l.conns[fd].state == .closing) {
				l.push_output(fd)
				if l.conns[fd].state == .free {
					continue
				}
			}
			if flags & u32(C.EPOLLIN) != 0 && l.conns[fd].state == .open {
				l.read_conn(fd, now)
			} else if flags & u32(C.EPOLLRDHUP) != 0 && l.conns[fd].state == .open {
				l.close_conn(fd)
			}
		}
		l.finish_batch(now)
		if now - l.last_sweep >= sweep_every_s {
			l.last_sweep = now
			l.sweep(now)
		}
	}
}

fn (mut l Loop) accept_burst() {
	for _ in 0 .. accept_burst {
		if l.n_open >= l.max_open {
			l.pause_accept()
			return
		}
		fd := C.accept4(l.lfd, unsafe { nil }, unsafe { nil }, C.SOCK_NONBLOCK | C.SOCK_CLOEXEC)
		if fd < 0 {
			e := C.errno
			if e == C.EMFILE || e == C.ENFILE {
				l.pause_accept()
			}
			return
		}
		opt := 1
		C.setsockopt(fd, C.IPPROTO_TCP, C.TCP_NODELAY, &opt, sizeof(int))
		if fd >= l.conns.len {
			mut nl := l.conns.len
			for nl <= fd {
				nl *= 2
			}
			for l.conns.len < nl {
				l.conns << Conn{}
			}
		}
		mut c := &l.conns[fd]
		c.generation++
		c.state = .open
		c.touched = l.seconds()
		c.sent = 0
		if epoll_add(l.epfd, fd, ev_in) < 0 {
			c.state = .free
			C.close(fd)
			continue
		}
		l.n_open++
	}
}

// pause_accept stops watching the listener until a connection closes, so a full fd table
// does not spin the loop on an always-readable listening socket.
fn (mut l Loop) pause_accept() {
	if l.accepting {
		l.accepting = false
		C.epoll_ctl(l.epfd, C.EPOLL_CTL_DEL, l.lfd, unsafe { nil })
	}
}

fn (mut l Loop) resume_accept() {
	if !l.accepting && l.n_open < l.max_open {
		l.accepting = true
		epoll_add(l.epfd, l.lfd, u32(C.EPOLLIN))
	}
}

fn (mut l Loop) close_conn(fd int) {
	mut c := &l.conns[fd]
	if c.state == .free {
		return
	}
	C.close(fd) // also removes fd from the epoll set
	l.buf_drop(mut c.input)
	l.buf_drop(mut c.output)
	c.sent = 0
	c.state = .free
	l.n_open--
	l.resume_accept()
}

// read_conn reads what the socket has, answers every complete request into the batch
// arena and keeps an incomplete tail in the connection's input buffer.
fn (mut l Loop) read_conn(fd int, now u32) {
	mut c := &l.conns[fd]
	mut have := int(c.input.len)
	if have > 0 {
		unsafe { vmemcpy(l.scratch.data, c.input.ptr, have) }
	}
	mut eof := false
	for {
		room := scratch_size - have
		if room == 0 {
			break
		}
		n := C.recv(fd, unsafe { &u8(l.scratch.data) + have }, usize(room), 0)
		if n > 0 {
			have += int(n)
			if int(n) < room {
				break
			}
			continue
		}
		if n == 0 {
			eof = true
		} else if C.errno != C.EAGAIN && C.errno != C.EINTR {
			eof = true
		}
		break
	}
	mut pos := 0
	mut served := 0
	for pos < have && served < max_requests_per_pass {
		total := fasthttp.frame_request_length(l.scratch[pos..have]) or {
			l.bad_request(fd)
			return
		}
		if total < 0 {
			break
		}
		req := fasthttp.decode_http_request(l.scratch[pos..pos + total]) or {
			l.bad_request(fd)
			return
		}
		start := l.out.len
		oc := l.app.handle(req, mut l.out)
		l.segs << Seg{
			fd:    fd
			start: start
			end:   l.out.len
			wrote: oc.wrote
		}
		if oc.close {
			c.state = .closing
		}
		pos += total
		served++
	}
	c.touched = now
	rest := have - pos
	if rest > 0 && c.state != .closing {
		if rest >= scratch_size {
			l.bad_request(fd) // a single request larger than the scratch buffer
			return
		}
		if pos > 0 || c.input.len == 0 {
			c.input.len = 0
			l.buf_append(mut c.input, unsafe { &u8(l.scratch.data) + pos }, rest)
		} else {
			// nothing new was consumed: the old tail is already in input; append the new bytes
			old := int(c.input.len)
			if have > old {
				l.buf_append(mut c.input, unsafe { &u8(l.scratch.data) + old }, have - old)
			}
		}
	} else if c.input.len > 0 {
		l.buf_drop(mut c.input)
	}
	if eof {
		if l.segs.len > 0 && l.segs[l.segs.len - 1].fd == fd {
			c.state = .closing // flush the answers of this batch, then close
		} else if c.output.len == 0 {
			l.close_conn(fd)
		} else {
			c.state = .closing
		}
	}
}

fn (mut l Loop) bad_request(fd int) {
	mut c := &l.conns[fd]
	start := l.out.len
	put_error(mut l.out, status_400, 'bad request', true)
	l.segs << Seg{
		fd:    fd
		start: start
		end:   l.out.len
	}
	c.state = .closing
	l.buf_drop(mut c.input)
}

// finish_batch commits the batch, then sends every response of the batch.
fn (mut l Loop) finish_batch(now u32) {
	if l.segs.len == 0 {
		return
	}
	committed := l.app.batch_end()
	for seg in l.segs {
		mut c := &l.conns[seg.fd]
		if c.state == .free {
			continue
		}
		if !committed && seg.wrote {
			// the write was rolled back: do not answer with a 201 that is not true
			c.state = .closing
			l.buf_drop(mut c.output)
			c.sent = 0
			l.close_conn(seg.fd)
			continue
		}
		p := unsafe { &u8(l.out.data) + seg.start }
		n := seg.end - seg.start
		if c.output.len > 0 {
			l.buf_append(mut c.output, p, n)
			continue
		}
		w := C.send(seg.fd, p, usize(n), C.MSG_NOSIGNAL)
		if w == n {
			continue
		}
		if w < 0 && C.errno != C.EAGAIN && C.errno != C.EINTR {
			l.close_conn(seg.fd)
			continue
		}
		done := if w > 0 { int(w) } else { 0 }
		l.buf_append(mut c.output, unsafe { p + done }, n - done)
		c.sent = 0
		if c.state == .open {
			c.state = .parked
			epoll_mod(l.epfd, seg.fd, ev_out)
		} else if c.state == .closing {
			epoll_mod(l.epfd, seg.fd, ev_out)
		}
	}
	for seg in l.segs {
		mut c := &l.conns[seg.fd]
		if c.state == .closing && c.output.len == 0 {
			l.close_conn(seg.fd)
		}
	}
	l.segs.clear()
	unsafe {
		l.out.len = 0
	}
	_ = now
}

// push_output continues a parked send; once drained the connection reads again.
fn (mut l Loop) push_output(fd int) {
	mut c := &l.conns[fd]
	for c.sent < c.output.len {
		w := C.send(fd, unsafe { c.output.ptr + c.sent }, usize(c.output.len - c.sent), C.MSG_NOSIGNAL)
		if w > 0 {
			c.sent += u32(w)
			continue
		}
		if w < 0 && (C.errno == C.EAGAIN || C.errno == C.EINTR) {
			return
		}
		l.close_conn(fd)
		return
	}
	l.buf_drop(mut c.output)
	c.sent = 0
	c.touched = l.seconds()
	if c.state == .closing {
		l.close_conn(fd)
		return
	}
	c.state = .open
	epoll_mod(l.epfd, fd, ev_in)
}

// sweep closes connections that have been stuck mid-request or mid-response for too long.
// Idle keep-alive connections (nothing pending) are never timed out.
fn (mut l Loop) sweep(now u32) {
	for fd in 0 .. l.conns.len {
		c := &l.conns[fd]
		if c.state == .free {
			continue
		}
		if (c.input.len > 0 || c.output.len > 0) && now - c.touched >= stall_timeout_s {
			l.close_conn(fd)
		}
	}
}
