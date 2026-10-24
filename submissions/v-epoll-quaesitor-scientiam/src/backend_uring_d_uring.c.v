module main

import fasthttp
import os
import sync
import time

// The io_uring backend (`-d uring`, Linux 6.1+): one ring per thread, each with its own
// direct-descriptor table (so connections never consume process descriptors and the
// LimitNOFILE ceiling is per ring, not per process), multishot accept and receive with
// kernel-provided buffers, and batched completions. All rings share one App (one SQLite
// connection, exclusive locking) under a mutex: a ring's completion batch is handled,
// committed and only then flushed, exactly like the epoll backend.
//
// Per-connection flow control: while a send is in flight, later responses are queued
// behind it. Once more than ur_park_output is queued the connection is parked: its receive
// is cancelled (bytes already on the way are kept, up to ur_parked_input) until the queue
// has drained; then up to ur_max_requests_per_pass of the kept requests are served per pass
// and the receive is armed again. ur_max_output is the hard cap after which the connection
// is closed instead. Each batch's response arena is bounded by ur_max_arena: when it is
// reached the remaining completions wait for the next pass.

#flag -I@VMODROOT/src
#flag @VMODROOT/src/uring_shim.c
#flag -luring
#include "uring_shim.h"

struct C.us_ring {}

struct C.us_cqe {
mut:
	res       int
	flags     u32
	user_data u64
}

fn C.us_new(u32, u32, u32, u32, u32, &int) &C.us_ring
fn C.us_free(&C.us_ring)
fn C.us_accept_multishot(&C.us_ring, int, u64) int
fn C.us_recv_multishot(&C.us_ring, int, u64) int
fn C.us_send(&C.us_ring, int, voidptr, u32, u64) int
fn C.us_close(&C.us_ring, int, u64) int
fn C.us_cancel(&C.us_ring, u64, u64) int
fn C.us_cancel_slot(&C.us_ring, int, u64) int
fn C.us_submit_wait(&C.us_ring, u32, int) int
fn C.us_peek(&C.us_ring, &C.us_cqe, u32) u32
fn C.us_advance(&C.us_ring, u32)
fn C.us_buf(&C.us_ring, u32) &u8
fn C.us_buf_return(&C.us_ring, u32)

const us_f_buffer = u32(1)
const us_f_more = u32(2)
const ur_sq_entries = u32(4096)
const ur_cq_entries = u32(16384)
const ur_buf_count = u32(4096)
const ur_buf_size = u32(2048)
const ur_batch = u32(32)
const ur_wait_busy_us = 250
const ur_wait_idle_us = 1_000_000
const ur_max_cqes = 512
// A connection is parked once ur_park_output is queued behind its send. Before the parking
// takes effect one batch (bounded by ur_max_arena) plus one receive buffer of responses can
// still be queued, so the hard cap sits above that.
const ur_max_output = u32(4 << 20)
const ur_park_output = u32(64 * 1024)
const ur_max_arena = 2 << 20
const ur_max_input = u32(16 * 1024) // a single request larger than this is refused
const ur_parked_input = u32(1 << 20) // bytes kept while parked (the kernel had them already)
const ur_max_requests_per_pass = 64
const ur_unlimited = int(0x7fffffff)

const op_accept = u8(1)
const op_recv = u8(2)
const op_send = u8(3)
const op_close = u8(4)
const op_cancel = u8(5)

@[inline]
fn tag(op u8, slot int, gen u32) u64 {
	return u64(op) | u64(slot) << 8 | u64(gen) << 32
}

@[inline]
fn tag_op(t u64) u8 {
	return u8(t & 0xff)
}

@[inline]
fn tag_slot(t u64) int {
	return int(t >> 8 & 0xffffff)
}

@[inline]
fn tag_gen(t u64) u32 {
	return u32(t >> 32)
}

// Shared is what every ring thread shares: the App behind one mutex, and the listener.
@[heap]
struct Shared {
mut:
	app &App = unsafe { nil }
	mu  &sync.Mutex
	lfd int
}

// Ring is one thread's io_uring loop.
@[heap]
struct Ring {
mut:
	id         int
	sh         &Shared    = unsafe { nil }
	ring       &C.us_ring = unsafe { nil }
	slots      int
	conns      []Conn
	n_open     int
	cqes       []C.us_cqe
	out        []u8 // every response of the current batch
	segs       []Seg
	pool       BufPool
	rearm      []u64 // recv tags of connections whose multishot receive ended and must be re-armed
	more       []u64 // recv tags of connections with complete requests still waiting in input
	accepting  bool
	reserve    int  // stop accepting when fewer slots than this are free (other rings take over)
	busy       bool // the previous batch was large: wait for a full batch, briefly
	start_ns   i64
	last_sweep u32
	// anomaly counters, printed by sweep when URING_DEBUG is set
	n_ms_end     int // multishot receives that ended with data (re-armed)
	n_enobufs    int
	n_reuse      int // accepted into a slot whose close completion was still in flight
	n_stale_cls  int // close completions ignored for that reason
	n_rearm_fail int
	n_queued     int // responses queued behind a send in flight
	n_short      int // short sends
	n_wait_err   int // submit_wait errors other than EINTR/EBUSY (the first few are logged)
	debug        bool
	trace        bool
}

fn (r &Ring) seconds() u32 {
	return u32((time.sys_mono_now() - r.start_ns) / time.second)
}

// run_uring serves HOST:PORT on RINGS threads (default 8, like the reference entry under
// the 65,535 descriptor limit) until the process is stopped.
fn run_uring(host string, port int) ! {
	// probe once on this thread so a kernel or sandbox without io_uring reports an error the
	// caller can fall back from, instead of a thread dying later. This runs before the
	// database is opened: its exclusive lock would otherwise block the fallback's own open.
	mut perr := 0
	probe := C.us_new(64, 128, 64, 16, ur_buf_size, &perr)
	if probe == unsafe { nil } {
		return error('io_uring unavailable (errno ${-perr})')
	}
	C.us_free(probe)
	// one process owns the file exclusively; with SERVER_PROCS > 1 (see run_epoll) each
	// statement autocommits and the busy timeout covers contention
	procs := os.getenv('SERVER_PROCS').int()
	single := procs <= 1 && os.getenv('DB_EXCLUSIVE') != '0'
	mut app := new_app(single, single)!
	mut rl := C.rlimit{}
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	rl.rlim_cur = rl.rlim_max
	C.setrlimit(C.RLIMIT_NOFILE, &rl)
	C.getrlimit(C.RLIMIT_NOFILE, &rl)
	// the kernel caps a ring's registered-file table at RLIMIT_NOFILE
	mut slots := int(rl.rlim_cur)
	if slots > 524280 {
		slots = 524280
	}
	mut rings := os.getenv('RINGS').int()
	if rings <= 0 {
		rings = (8 * 65535 + slots - 1) / slots
		if rings > 8 {
			rings = 8
		}
		if rings < 1 {
			rings = 1
		}
	}
	lfd := create_listener(host, port) or {
		app.db.close() // the fallback opens the file again
		return err
	}
	mut sh := &Shared{
		app: app
		mu:  sync.new_mutex()
		lfd: lfd
	}
	assert sizeof(Conn) == 64
	println('listening on http://${host}:${port}/ (io_uring backend, pid ${os.getpid()}, ${rings} rings x ${slots} slots)')
	flush_stdout()
	mut threads := []thread{cap: rings}
	for i in 0 .. rings {
		threads << spawn ring_thread(sh, i, slots)
	}
	threads.wait()
}

fn ring_thread(sh &Shared, id int, slots int) {
	mut err := 0
	ring := C.us_new(ur_sq_entries, ur_cq_entries, u32(slots), ur_buf_count, ur_buf_size,
		&err)
	if ring == unsafe { nil } {
		eprintln('fatal: io_uring setup failed on ring ${id}: errno ${-err}')
		exit(1)
	}
	mut r := &Ring{
		id:       id
		sh:       unsafe { sh }
		ring:     ring
		slots:    slots
		conns:    []Conn{len: 4096}
		cqes:     []C.us_cqe{len: ur_max_cqes}
		out:      []u8{cap: 1 << 20}
		segs:     []Seg{cap: 1024}
		rearm:    []u64{cap: 256}
		more:     []u64{cap: 64}
		reserve:  if slots / 8 < 1024 { slots / 8 } else { 1024 }
		start_ns: time.sys_mono_now()
		debug:    os.getenv('URING_DEBUG') != ''
		trace:    os.getenv('URING_DEBUG') == '2'
	}
	r.run()
}

fn (mut r Ring) conn(slot int) &Conn {
	if slot >= r.conns.len {
		mut nl := r.conns.len
		for nl <= slot {
			nl *= 2
		}
		for r.conns.len < nl {
			r.conns << Conn{}
		}
	}
	return &r.conns[slot]
}

fn (mut r Ring) run() {
	for {
		mut wait_nr := if r.busy { ur_batch } else { u32(1) }
		mut timeout := if r.busy { ur_wait_busy_us } else { ur_wait_idle_us }
		if r.more.len > 0 {
			// requests are already waiting in memory: submit, collect what is ready, go on
			wait_nr = 0
			timeout = 0
		}
		ready := C.us_submit_wait(r.ring, wait_nr, timeout)
		if ready < 0 {
			if ready == -C.EINTR {
				continue
			}
			// -EBUSY: the completion ring overflowed and must be drained before the kernel
			// accepts more submissions, so fall through to the peek loop instead of retrying
			if ready != -C.EBUSY {
				r.n_wait_err++
				if r.n_wait_err <= 8 {
					eprintln('ring ${r.id}: submit_wait: errno ${-ready}')
				}
			}
		}
		now := r.seconds()
		mut total := 0
		for {
			n := int(C.us_peek(r.ring, &r.cqes[0], u32(r.cqes.len)))
			if n == 0 {
				break
			}
			r.sh.mu.lock()
			r.sh.app.tick()
			mut done := 0
			for i in 0 .. n {
				r.dispatch(r.cqes[i], now)
				done++
				if r.out.len >= ur_max_arena {
					break // bound the batch; the remaining completions are peeked again
				}
			}
			r.finish_batch()
			r.sh.mu.unlock()
			C.us_advance(r.ring, u32(done))
			total += done
			if done == n && n < r.cqes.len {
				break
			}
		}
		if r.more.len > 0 {
			// pipelined requests left over from an earlier pass (the per-pass cap)
			pending := r.more.clone()
			r.more.clear()
			r.sh.mu.lock()
			r.sh.app.tick()
			for t in pending {
				slot := tag_slot(t)
				mut c := r.conn(slot)
				if c.state == .open && c.generation == tag_gen(t) && c.input.len > 0 {
					_ = r.on_data(slot, mut c, unsafe { nil }, 0, now, ur_max_requests_per_pass)
				}
			}
			r.finish_batch()
			r.sh.mu.unlock()
		}
		r.busy = total >= 4 || r.rearm.len > 0
		free_slots := r.slots - r.n_open
		if !r.accepting && free_slots > 2 * r.reserve {
			if C.us_accept_multishot(r.ring, r.sh.lfd, tag(op_accept, 0, 0)) == 0 {
				r.accepting = true
			}
		} else if r.accepting && free_slots < r.reserve {
			// nearly full: let the other rings take the next connections
			C.us_cancel(r.ring, tag(op_accept, 0, 0), tag(op_cancel, 0, 0))
			r.accepting = false
		}
		if r.rearm.len > 0 {
			mut again := []u64{}
			for t in r.rearm {
				slot := tag_slot(t)
				mut c := r.conn(slot)
				// a connection closed and replaced since the entry was made must not get a
				// second receive (its accept armed one): the generation tells them apart
				if c.state == .open && c.generation == tag_gen(t)
					&& C.us_recv_multishot(r.ring, slot, t) != 0 {
					again << t
					r.n_rearm_fail++
				}
			}
			r.rearm.clear()
			r.rearm << again
		}
		if now - r.last_sweep >= sweep_every_s {
			r.last_sweep = now
			r.sweep(now)
		}
	}
}

fn (mut r Ring) dispatch(cqe C.us_cqe, now u32) {
	if r.trace {
		eprintln('cqe op=${tag_op(cqe.user_data)} slot=${tag_slot(cqe.user_data)} gen=${tag_gen(cqe.user_data)} res=${cqe.res} flags=${cqe.flags:x}')
	}
	op := tag_op(cqe.user_data)
	if op == op_cancel {
		return
	}
	if op == op_accept {
		if cqe.flags & us_f_more == 0 && cqe.res != -C.ECANCELED {
			r.accepting = false
		}
		if cqe.res < 0 {
			return
		}
		slot := cqe.res
		mut c := r.conn(slot)
		if c.state != .free {
			r.n_reuse++
			// the previous owner's close completion is still in flight (completions of
			// different operations are not ordered): the slot is ours now, its record is reset
			r.pool.drop(mut c.input)
			r.pool.drop(mut c.output)
			r.pool.drop(mut c.flight)
			r.n_open--
		}
		c.generation++
		c.state = .open
		c.touched = now
		c.sent = 0
		r.n_open++
		if C.us_recv_multishot(r.ring, slot, tag(op_recv, slot, c.generation)) != 0 {
			r.rearm << tag(op_recv, slot, c.generation)
		}
		return
	}
	slot := tag_slot(cqe.user_data)
	gen := tag_gen(cqe.user_data)
	mut c := r.conn(slot)
	if op == op_close {
		if c.generation != gen || c.state != .closed {
			r.n_stale_cls++
			return // a new connection already owns the slot (see the accept path)
		}
		// the slot is free again (the kernel may hand it to a new accept from now on)
		r.pool.drop(mut c.input)
		r.pool.drop(mut c.output)
		r.pool.drop(mut c.flight)
		c.sent = 0
		c.state = .free
		r.n_open--
		return
	}
	if c.generation != gen || c.state == .free || c.state == .closed {
		if cqe.flags & us_f_buffer != 0 {
			C.us_buf_return(r.ring, cqe.flags >> 16)
		}
		return
	}
	match op {
		op_recv {
			if c.state == .closing {
				// whatever else the peer sends is not served (the epoll loop stops reading such
				// a connection too); its send completion will close it
				if cqe.flags & us_f_buffer != 0 {
					C.us_buf_return(r.ring, cqe.flags >> 16)
				}
				return
			}
			if c.state == .parked {
				// the receive is being cancelled; keep what still arrives for later, and note an
				// EOF. The cancellation itself (res < 0) needs nothing.
				if cqe.res > 0 {
					bid := cqe.flags >> 16
					if c.input.len + u32(cqe.res) > ur_parked_input {
						r.close(slot, mut c)
					} else {
						r.pool.append(mut c.input, unsafe { C.us_buf(r.ring, bid) }, cqe.res)
					}
					C.us_buf_return(r.ring, bid)
				} else if cqe.res == 0 {
					if cqe.flags & us_f_buffer != 0 {
						C.us_buf_return(r.ring, cqe.flags >> 16)
					}
					r.on_eof(slot, mut c, now)
				}
				return
			}
			if cqe.res > 0 {
				bid := cqe.flags >> 16
				data := unsafe { C.us_buf(r.ring, bid) }
				// everything complete in the buffer is served: a buffer is 2 KiB, and the batch
				// arena and parking bound what a burst can do
				_ = r.on_data(slot, mut c, data, cqe.res, now, ur_unlimited)
				C.us_buf_return(r.ring, bid)
				if cqe.flags & us_f_more == 0 && c.state == .open {
					r.n_ms_end++
					r.rearm << cqe.user_data
				}
			} else if cqe.res == 0 {
				if cqe.flags & us_f_buffer != 0 {
					C.us_buf_return(r.ring, cqe.flags >> 16)
				}
				r.on_eof(slot, mut c, now)
			} else if cqe.res == -C.ENOBUFS {
				r.n_enobufs++
				r.rearm << cqe.user_data
			} else if cqe.res == -C.ECANCELED {
				// the parked receive's cancellation, completed after the send that unparked the
				// connection (the kernel posts it from deferred task work): the live receive is
				// the one armed at the unpark
			} else {
				r.close(slot, mut c)
			}
		}
		op_send {
			if cqe.res < 0 {
				r.close(slot, mut c)
				return
			}
			c.sent += u32(cqe.res)
			c.touched = now
			if c.sent < c.flight.len {
				r.n_short++
				r.send_flight(slot, mut c)
				return
			}
			r.pool.drop(mut c.flight)
			c.sent = 0
			if c.output.len > 0 {
				c.flight = c.output
				c.output = Buf{}
				r.send_flight(slot, mut c)
				return
			}
			if c.state == .parked {
				// drained: serve what arrived meanwhile, then receive again (the cancelled
				// receive is gone: its cancel was submitted before this completion was waited for)
				c.state = .open
				if c.input.len > 0 {
					_ = r.on_data(slot, mut c, unsafe { nil }, 0, now, ur_max_requests_per_pass)
				}
				r.rearm << tag(op_recv, slot, c.generation)
				return
			}
			if c.state == .closing && !r.owed_in_batch(slot) {
				// nothing left to send (responses of the current batch, if any, are still in
				// r.segs; finish_batch sends them and their completion closes the connection)
				r.close(slot, mut c)
			}
		}
		else {}
	}
}

// owed_in_batch reports whether the current batch holds a response for slot.
fn (r &Ring) owed_in_batch(slot int) bool {
	for i := r.segs.len - 1; i >= 0; i-- {
		if r.segs[i].fd == slot {
			return true
		}
	}
	return false
}

fn (mut r Ring) send_flight(slot int, mut c Conn) {
	if C.us_send(r.ring, slot, unsafe { c.flight.ptr + c.sent }, c.flight.len - c.sent,
		tag(op_send, slot, c.generation)) != 0 {
		r.close(slot, mut c)
	}
}

// close cancels whatever is pending on the slot (a receive waiting for data, a send waiting for
// room: the kernel keeps the socket open until they complete) and submits the close; the slot
// is reusable once the close completion arrives.
fn (mut r Ring) close(slot int, mut c Conn) {
	if c.state == .closed || c.state == .free {
		return
	}
	c.state = .closed
	C.us_cancel_slot(r.ring, slot, tag(op_cancel, slot, c.generation))
	C.us_close(r.ring, slot, tag(op_close, slot, c.generation))
}

// on_data answers up to limit complete requests from the received bytes (plus whatever is
// kept in input from earlier receives) into the batch arena and keeps the rest in input:
// an incomplete request, or complete requests beyond the limit, which Ring.more serves on
// the next pass. With n == 0 it serves from input alone. It returns the number served.
fn (mut r Ring) on_data(slot int, mut c Conn, data &u8, n int, now u32, limit int) int {
	mut buf := []u8{}
	if c.input.len > 0 {
		if n > 0 {
			r.pool.append(mut c.input, data, n)
		}
		buf = unsafe {
			array{
				data:         c.input.ptr
				len:          int(c.input.len)
				cap:          int(c.input.cap)
				element_size: 1
			}
		}
	} else {
		buf = unsafe {
			array{
				data:         data
				len:          n
				cap:          n
				element_size: 1
			}
		}
	}
	mut pos := 0
	mut served := 0
	for pos < buf.len && served < limit {
		total := fasthttp.frame_request_length(buf[pos..]) or {
			r.bad_request(slot, mut c)
			return served
		}
		if total < 0 {
			break
		}
		req := fasthttp.decode_http_request(buf[pos..pos + total]) or {
			r.bad_request(slot, mut c)
			return served
		}
		if r.trace {
			r.trace_req(slot, buf[pos..pos + total])
		}
		start := r.out.len
		oc := r.sh.app.handle(req, mut r.out)
		r.segs << Seg{
			fd:    slot
			start: start
			end:   r.out.len
			wrote: oc.wrote
		}
		if oc.close {
			c.state = .closing
		}
		pos += total
		served++
	}
	c.touched = now
	rest := buf.len - pos
	if rest > 0 && pos == 0 && served == 0 && u32(rest) > ur_max_input {
		r.bad_request(slot, mut c) // a single request larger than any this API takes
		return served
	}
	if rest > 0 && (c.state == .open || c.state == .parked) {
		if c.input.len > 0 {
			// the tail is already in input: move it to the front
			if pos > 0 {
				unsafe { vmemmove(c.input.ptr, c.input.ptr + pos, rest) }
				c.input.len = u32(rest)
			}
		} else {
			r.pool.append(mut c.input, unsafe { data + pos }, rest)
		}
		if served >= limit && c.state == .open {
			// the limit stopped the loop with complete requests left: no receive completion
			// will come for them, so serve them on the next pass (a parked connection's are
			// served when it drains)
			r.more << tag(op_recv, slot, c.generation)
		}
	} else if c.input.len > 0 {
		r.pool.drop(mut c.input)
	}
	return served
}

// on_eof serves the next requests still buffered (a pass at a time, so a backlog cannot
// overflow the output queue), coming back for the rest through a re-armed receive, which
// reports the EOF again, or through the unpark of a parked connection; an incomplete tail
// is forgotten. Once nothing is buffered it closes the connection as soon as everything it
// is owed has been sent: responses still in this batch (its receive completions can be
// interleaved with other connections'), in flight, or queued.
fn (mut r Ring) on_eof(slot int, mut c Conn, now u32) {
	if c.state != .open && c.state != .parked {
		return
	}
	if c.input.len > 0 {
		served := r.on_data(slot, mut c, unsafe { nil }, 0, now, ur_max_requests_per_pass)
		if c.state == .closing || c.state == .closed {
			return // a bad request already decided the close
		}
		if c.input.len > 0 {
			if served > 0 {
				if c.state == .open {
					r.rearm << tag(op_recv, slot, c.generation)
				}
				return
			}
			r.pool.drop(mut c.input) // only an incomplete request is left
		}
	}
	if c.flight.len > 0 || c.output.len > 0 || r.owed_in_batch(slot) {
		c.state = .closing
	} else {
		r.close(slot, mut c)
	}
}

fn (mut r Ring) bad_request(slot int, mut c Conn) {
	start := r.out.len
	put_error(mut r.out, status_400, 'bad request', true)
	r.segs << Seg{
		fd:    slot
		start: start
		end:   r.out.len
	}
	c.state = .closing
	r.pool.drop(mut c.input)
}

// finish_batch commits the batch, then hands every response of the batch to the socket.
fn (mut r Ring) finish_batch() {
	if r.segs.len == 0 {
		return
	}
	committed := r.sh.app.batch_end()
	for seg in r.segs {
		mut c := &r.conns[seg.fd]
		if c.state == .free || c.state == .closed {
			continue
		}
		if !committed {
			// The batch was rolled back: a 201 would be a lie, and a read in the same batch may
			// have seen the rolled-back rows, so no response of this batch leaves the process.
			r.pool.drop(mut c.output)
			r.close(seg.fd, mut c)
			continue
		}
		p := unsafe { &u8(r.out.data) + seg.start }
		n := seg.end - seg.start
		if c.flight.len > 0 {
			// a send is in flight: queue behind it
			if c.output.len + u32(n) > ur_max_output {
				r.close(seg.fd, mut c)
				continue
			}
			r.pool.append(mut c.output, p, n)
			r.n_queued++
			if c.state == .open && c.output.len > ur_park_output {
				// the peer is not reading: stop receiving until the queue has drained
				c.state = .parked
				C.us_cancel(r.ring, tag(op_recv, seg.fd, c.generation), tag(op_cancel, seg.fd,
					c.generation))
			}
			continue
		}
		if c.output.len > 0 {
			r.pool.append(mut c.output, p, n)
			c.flight = c.output
			c.output = Buf{}
		} else {
			r.pool.append(mut c.flight, p, n)
		}
		c.sent = 0
		r.send_flight(seg.fd, mut c)
	}
	r.segs.clear()
	unsafe {
		r.out.len = 0
	}
}

// sweep closes connections stuck mid-request or mid-response for too long. Idle
// keep-alive connections are never timed out.
fn (mut r Ring) sweep(now u32) {
	if r.debug {
		eprintln('ring ${r.id}: open=${r.n_open} ms_end=${r.n_ms_end} enobufs=${r.n_enobufs} reuse=${r.n_reuse} stale_close=${r.n_stale_cls} rearm_fail=${r.n_rearm_fail} queued=${r.n_queued} short=${r.n_short} wait_err=${r.n_wait_err}')
	}
	for slot in 0 .. r.conns.len {
		mut c := &r.conns[slot]
		if c.state == .free || c.state == .closed {
			continue
		}
		if (c.input.len > 0 || c.output.len > 0 || c.flight.len > 0)
			&& now - c.touched >= stall_timeout_s {
			r.close(slot, mut c)
		}
	}
}

// trace_req logs the request line and X-Id header of a request (URING_DEBUG=2).
fn (r &Ring) trace_req(slot int, req []u8) {
	mut line_end := req.len
	for i in 0 .. req.len {
		if req[i] == `\r` {
			line_end = i
			break
		}
	}
	mut xid := ''
	if idx := req.bytestr().to_lower().index('x-id:') {
		rest := req[idx + 5..]
		mut e := 0
		for e < rest.len && rest[e] != `\r` {
			e++
		}
		xid = rest[..e].bytestr().trim_space()
	}
	eprintln('req slot=${slot} xid=${xid} ${req[..line_end].bytestr()}')
}
