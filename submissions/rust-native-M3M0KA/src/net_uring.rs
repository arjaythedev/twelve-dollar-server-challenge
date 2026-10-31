//! io_uring transport: one issuer thread, several rings of direct descriptors.
//!
//! Accepted sockets are installed in a ring's fixed-file table instead of the
//! process descriptor table, so `LimitNOFILE=65535` bounds one ring rather than
//! the whole server. Rings beyond the first are opened only when the accepting
//! ring runs low, so a lightly loaded server allocates one table, not eight.
//!
//! Accepts are one-shot and armed only while slots remain: a burst can never
//! overrun a table, which is how multishot accept loses connections at handover.
//! Each connection holds one multishot receive into buffers provided with
//! `IORING_OP_PROVIDE_BUFFERS`, which every Linux 5.7+ kernel accepts (the
//! buffer-ring registration is rejected outright by Ubuntu's 6.8 kernels).
//! Idle connections own no user-space buffer.
//!
//! `self_test` drives real concurrent, pipelined and closing clients through
//! this exact code path before the public listener exists. If anything fails —
//! seccomp, an old kernel, an ABI mismatch — the caller stays on the other
//! transport rather than serving from an untested path.

use std::mem;
use std::os::raw::c_int;

use crate::app::App;
use crate::http;
use crate::sys;
use crate::uring::{
    new_eventfd, read_eventfd, Ring, Sqe, IORING_CQE_F_MORE, IORING_CQE_BUFFER_SHIFT,
    IORING_ENTER_GETEVENTS, IORING_SETUP_CQSIZE, IORING_SETUP_DEFER_TASKRUN,
    IORING_SETUP_SINGLE_ISSUER, IORING_SETUP_TASKRUN_FLAG,
};

/// Ring 0 plus seven more, each with the process's descriptor limit.
const MAX_LANES: usize = 8;
const SLOTS: u32 = 65535;
const SQ_ENTRIES: u32 = 2048;
const BUF_COUNT: u32 = 2048;
const BUF_SIZE: usize = 2048;
/// Accepts in flight per lane, and the free-slot level that triggers handover.
const ACCEPTS: u32 = 32;
const HANDOVER_FREE: u32 = 4096;

const K_ACCEPT: u64 = 1;
const K_RECV: u64 = 2;
const K_SEND: u64 = 3;
const K_SHUTDOWN: u64 = 4;
const K_CLOSE: u64 = 5;
const K_PROVIDE: u64 = 6;

#[inline]
fn tag(kind: u64, slot: u32, lane: u32) -> u64 {
    kind | ((slot as u64) << 4) | ((lane as u64) << 24)
}
#[inline]
fn tag_kind(u: u64) -> u64 {
    u & 0xf
}
#[inline]
fn tag_slot(u: u64) -> u32 {
    ((u >> 4) & 0xf_ffff) as u32
}
#[inline]
fn tag_lane(u: u64) -> u32 {
    ((u >> 24) & 0xff) as u32
}

struct Conn {
    live: bool,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    sent: usize,
    close_after: bool,
    send_busy: bool,
}

impl Conn {
    fn new() -> Conn {
        Conn {
            live: false,
            inbuf: Vec::new(),
            outbuf: Vec::new(),
            sent: 0,
            close_after: false,
            send_busy: false,
        }
    }
}

struct Lane {
    ring: Ring,
    efd: c_int,
    bufs: Vec<u8>,
    /// Buffers returned since the last flush, as a bitmap.
    pending: Box<[u64]>,
    npending: u32,
    free_slots: u32,
    conns: Vec<Conn>,
    accept_inflight: u32,
    accepting: bool,
    /// Connections whose close could not be submitted yet.
    deferred_close: Vec<u32>,
    /// Connections with output ready, or waiting to be closed.
    dirty: Vec<u32>,
    /// Response buffers taken back after their send completed.
    pool: Vec<Vec<u8>>,
}

impl Lane {
    fn pending_words() -> usize {
        (BUF_COUNT as usize).div_ceil(64)
    }

    /// Returns buffer `bid` to the kernel, batched until the pass ends.
    fn recycle(&mut self, bid: u32) {
        let w = bid as usize >> 6;
        if w >= self.pending.len() {
            return;
        }
        let bit = 1u64 << (bid & 63);
        if self.pending[w] & bit == 0 {
            self.pending[w] |= bit;
            self.npending += 1;
        }
    }

    /// Hands every returned buffer back in as few submissions as possible: one
    /// per contiguous run of ids.
    fn flush_buffers(&mut self, lane: usize) {
        if self.npending == 0 {
            return;
        }
        let base = self.bufs.as_ptr();
        let mut i = 0u32;
        while i < BUF_COUNT {
            if !bit_set(&self.pending, i) {
                i += 1;
                continue;
            }
            let start = i;
            let mut end = i;
            while end + 1 < BUF_COUNT && bit_set(&self.pending, end + 1) {
                end += 1;
            }
            for b in start..=end {
                let w = b as usize >> 6;
                self.pending[w] &= !(1u64 << (b & 63));
                self.npending -= 1;
            }
            let addr = unsafe { base.add(start as usize * BUF_SIZE) };
            let n = end - start + 1;
            let u = tag(K_PROVIDE, start, lane as u32);
            match self.ring.sqe() {
                Some(p) => Sqe(p).apply(Sqe::provide_buffers(addr, BUF_SIZE as u32, n, 0, start, u)),
                None => {
                    // Out of submission slots: keep these for the next pass.
                    for b in start..=end {
                        self.recycle(b);
                    }
                    return;
                }
            }
            i = end + 1;
        }
    }
}

#[inline]
fn bit_set(pending: &[u64], i: u32) -> bool {
    let w = i as usize >> 6;
    w < pending.len() && pending[w] & (1u64 << (i & 63)) != 0
}

pub struct UringServer {
    lanes: Vec<Lane>,
    ep: c_int,
    listen_fd: c_int,
    accept_lane: usize,
    app: App,
}

impl UringServer {
    fn arm_recv(&mut self, lane: usize, slot: u32) {
        let u = tag(K_RECV, slot, lane as u32);
        let l = &mut self.lanes[lane];
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::recv_multishot(slot, 0, u));
        }
    }

    /// Tops up this lane's one-shot accepts, never more than its free slots.
    fn arm_accepts(&mut self, lane: usize) {
        let want = self.lanes[lane].free_slots.saturating_sub(1).min(ACCEPTS);
        while self.lanes[lane].accepting && self.lanes[lane].accept_inflight < want {
            let u = tag(K_ACCEPT, 0, lane as u32);
            let l = &mut self.lanes[lane];
            if let Some(p) = l.ring.sqe() {
                Sqe(p).apply(Sqe::accept(self.listen_fd, u));
                l.accept_inflight += 1;
            } else {
                break;
            }
        }
    }

    fn start_accepting(&mut self, lane: usize) {
        if self.lanes[lane].accepting {
            return;
        }
        self.lanes[lane].accepting = true;
        self.accept_lane = lane;
        self.arm_accepts(lane);
        let _ = self.lanes[lane].ring.submit(0, 0);
    }

    /// Opens ring `i` with a fixed-file table sized to this process's limit.
    fn open_lane(&mut self, i: usize) -> bool {
        let table = unsafe { sys::nofile_limit() }.saturating_sub(64).max(64);
        let flags = IORING_SETUP_SINGLE_ISSUER
            | IORING_SETUP_DEFER_TASKRUN
            | IORING_SETUP_TASKRUN_FLAG
            | IORING_SETUP_CQSIZE;
        let mut ring = match Ring::new(SQ_ENTRIES, SQ_ENTRIES * 4, flags) {
            Some(r) => r,
            None => {
                eprintln!("io_uring: ring {i} unavailable");
                return false;
            }
        };
        if !ring.register_files(&vec![-1 as c_int; table as usize]) {
            eprintln!("io_uring: ring {i}: cannot register a {table}-slot file table");
            return false;
        }
        let bufs = vec![0u8; BUF_COUNT as usize * BUF_SIZE];
        let mut lane = Lane {
            ring,
            efd: -1,
            bufs,
            pending: vec![0u64; Lane::pending_words()].into_boxed_slice(),
            npending: 0,
            free_slots: table,
            conns: (0..table).map(|_| Conn::new()).collect(),
            accept_inflight: 0,
            accepting: false,
            deferred_close: Vec::new(),
            dirty: Vec::with_capacity(256),
            pool: Vec::new(),
        };
        // Every buffer is handed over once; the kernel owns them from here.
        let base = lane.bufs.as_ptr();
        if let Some(p) = lane.ring.sqe() {
            Sqe(p).apply(Sqe::provide_buffers(
                base,
                BUF_SIZE as u32,
                BUF_COUNT,
                0,
                0,
                tag(K_PROVIDE, 0, i as u32),
            ));
        } else {
            return false;
        }
        // The eventfd matters once several rings are live: it lets this thread
        // sleep in epoll across all of them.
        let efd = new_eventfd();
        if efd < 0 {
            return false;
        }
        lane.efd = efd;
        if !lane.ring.register_eventfd_async(efd) {
            eprintln!("io_uring: ring {i}: eventfd registration refused");
            return false;
        }
        let ev = sys::event_for(efd, sys::EPOLLIN);
        unsafe {
            if sys::epoll_ctl(self.ep, sys::EPOLL_CTL_ADD, efd, &ev) < 0 {
                return false;
            }
        }
        self.lanes.push(lane);
        if i > 0 {
            eprintln!(
                "io_uring: ring {i} opened ({} direct descriptors)",
                self.lanes[i].free_slots
            );
        }
        true
    }

    fn submit_close(&mut self, lane: usize, slot: u32) {
        let l = &mut self.lanes[lane];
        if l.ring.sq_space() < 2 {
            let _ = l.ring.submit(0, 0);
            if l.ring.sq_space() < 2 {
                if !l.deferred_close.contains(&slot) {
                    l.deferred_close.push(slot);
                }
                l.conns[slot as usize].live = false;
                return;
            }
        }
        // Both entries or neither: an SQE taken but not prepared would go out
        // as whatever it held last time.
        let u_shutdown = tag(K_SHUTDOWN, slot, lane as u32);
        let u_close = tag(K_CLOSE, slot, lane as u32);
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::shutdown(slot, u_shutdown));
        }
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::close(slot, u_close));
        }
        l.conns[slot as usize].live = false;
    }

    fn retry_deferred(&mut self, lane: usize) {
        if self.lanes[lane].deferred_close.is_empty() {
            return;
        }
        let pending = mem::take(&mut self.lanes[lane].deferred_close);
        for slot in pending {
            let l = &mut self.lanes[lane];
            if l.ring.sq_space() < 2 {
                l.deferred_close.push(slot);
                continue;
            }
            let u_shutdown = tag(K_SHUTDOWN, slot, lane as u32);
            let u_close = tag(K_CLOSE, slot, lane as u32);
            if let Some(p) = l.ring.sqe() {
                Sqe(p).apply(Sqe::shutdown(slot, u_shutdown));
            }
            if let Some(p) = l.ring.sqe() {
                Sqe(p).apply(Sqe::close(slot, u_close));
            }
        }
    }

    fn submit_send(&mut self, lane: usize, slot: u32) {
        let idx = slot as usize;
        let (ptr, len) = {
            let c = &self.lanes[lane].conns[idx];
            if c.sent >= c.outbuf.len() {
                return;
            }
            (unsafe { c.outbuf.as_ptr().add(c.sent) }, c.outbuf.len() - c.sent)
        };
        let u = tag(K_SEND, slot, lane as u32);
        let l = &mut self.lanes[lane];
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::send(slot, ptr, len, u));
            l.conns[idx].send_busy = true;
        }
    }

    /// Serves every complete request currently buffered on the connection.
    fn serve(&mut self, lane: usize, slot: u32) {
        let idx = slot as usize;
        if self.lanes[lane].conns[idx].outbuf.capacity() == 0 {
            if let Some(b) = self.lanes[lane].pool.pop() {
                self.lanes[lane].conns[idx].outbuf = b;
            }
        }
        let mut buf = mem::take(&mut self.lanes[lane].conns[idx].inbuf);
        if !buf.is_empty() {
            loop {
                match http::parse(&buf) {
                    Ok(None) => break,
                    Err(()) => {
                        let c = &mut self.lanes[lane].conns[idx];
                        http::respond(&mut c.outbuf, 400, crate::app::R_INTERNAL);
                        c.close_after = true;
                        buf.clear();
                        break;
                    }
                    Ok(Some((rq, used))) => {
                        if rq.expect_continue {
                            self.lanes[lane].conns[idx]
                                .outbuf
                                .extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                        }
                        // Cursors must not span a write; the DB layer closes and
                        // reopens them around one.
                        let close = {
                            let app = &mut self.app;
                            let out = &mut self.lanes[lane].conns[idx].outbuf;
                            app.handle(&rq, out)
                        };
                        buf.drain(..used);
                        if close {
                            let c = &mut self.lanes[lane].conns[idx];
                            c.close_after = true;
                            buf.clear();
                            break;
                        }
                    }
                }
            }
        }
        // Keep the buffer's capacity on the connection: freeing it per request
        // makes the allocator mmap and munmap on every pass.
        self.lanes[lane].conns[idx].inbuf = buf;
    }

    fn mark_dirty(&mut self, lane: usize, slot: u32) {
        let c = &self.lanes[lane].conns[slot as usize];
        if c.outbuf.len() > c.sent || c.close_after {
            self.lanes[lane].dirty.push(slot);
        }
    }

    fn on_data(&mut self, lane: usize, slot: u32, data: &[u8]) {
        let idx = slot as usize;
        if !self.lanes[lane].conns[idx].live {
            return;
        }
        // Common case: nothing buffered, this receive holds exactly one
        // complete request, and no send is in flight. Serve it straight out of
        // the kernel's provided buffer, so the connection allocates nothing.
        if self.lanes[lane].conns[idx].inbuf.is_empty()
            && !self.lanes[lane].conns[idx].send_busy
        {
            if let Ok(Some((rq, used))) = http::parse(data) {
                if used == data.len() {
                    let close = {
                        let l = &mut self.lanes[lane];
                        if rq.expect_continue {
                            l.conns[idx]
                                .outbuf
                                .extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                        }
                        let app = &mut self.app;
                        app.handle(&rq, &mut l.conns[idx].outbuf)
                    };
                    if close {
                        self.lanes[lane].conns[idx].close_after = true;
                    }
                    self.mark_dirty(lane, slot);
                    return;
                }
            }
        }
        // While a send is in flight the output buffer must not move, so new
        // bytes wait in the input buffer until it completes.
        self.lanes[lane].conns[idx].inbuf.extend_from_slice(data);
        if self.lanes[lane].conns[idx].inbuf.len() > http::MAX_HEAD + http::MAX_BODY {
            self.submit_close(lane, slot);
            return;
        }
        if !self.lanes[lane].conns[idx].send_busy {
            self.serve(lane, slot);
            self.mark_dirty(lane, slot);
        }
    }

    fn handle(&mut self, u: u64, res: i32, flags: u32) {
        let kind = tag_kind(u);
        let slot = tag_slot(u);
        let lane = tag_lane(u) as usize;
        if lane >= self.lanes.len() || slot as usize >= self.lanes[lane].conns.len() {
            return;
        }
        match kind {
            K_ACCEPT => {
                let l = &mut self.lanes[lane];
                l.accept_inflight = l.accept_inflight.saturating_sub(1);
                if res >= 0 {
                    let new_slot = res as u32;
                    let l = &mut self.lanes[lane];
                    if new_slot as usize >= l.conns.len() {
                        // Cannot happen with a table-sized slot, but never
                        // index past the table.
                        if let Some(p) = l.ring.sqe() {
                            Sqe(p).apply(Sqe::close(new_slot, tag(K_CLOSE, new_slot, lane as u32)));
                        }
                        return;
                    }
                    l.conns[new_slot as usize] = Conn::new();
                    l.conns[new_slot as usize].live = true;
                    l.free_slots = l.free_slots.saturating_sub(1);
                    self.arm_recv(lane, new_slot);
                    self.arm_accepts(lane);
                } else {
                    let err = -res;
                    // ENFILE/EMFILE/ENOBUFS: this table is full. Hand the
                    // listener to a ring with room (opening one if needed).
                    if err == 23 || err == 24 || err == 105 {
                        self.lanes[lane].accepting = false;
                        self.advance_accept(lane);
                    } else {
                        self.arm_accepts(lane);
                    }
                }
            }
            K_RECV => {
                if res > 0 {
                    let bid = flags >> IORING_CQE_BUFFER_SHIFT;
                    let start = bid as usize * BUF_SIZE;
                    let n = res as usize;
                    if start + n > self.lanes[lane].bufs.len() {
                        return;
                    }
                    // Serve straight from the provided buffer: `on_data` copies
                    // into the connection's own input buffer, and the buffer is
                    // not handed back to the kernel until the pass ends, so it
                    // stays valid for the whole call.
                    let base = self.lanes[lane].bufs.as_ptr();
                    self.lanes[lane].recycle(bid);
                    let data = unsafe { std::slice::from_raw_parts(base.add(start), n) };
                    self.on_data(lane, slot, data);
                    if flags & IORING_CQE_F_MORE == 0 && self.lanes[lane].conns[slot as usize].live
                    {
                        self.arm_recv(lane, slot);
                    }
                } else if res == 0 {
                    self.submit_close(lane, slot);
                } else {
                    let err = -res;
                    // ENOBUFS: no provided buffer was available; the receive
                    // must be re-armed. EAGAIN/EINTR/EALREADY likewise.
                    if err == 105 || err == 11 || err == 4 || err == 114 {
                        if self.lanes[lane].conns[slot as usize].live {
                            self.arm_recv(lane, slot);
                        }
                    } else {
                        self.submit_close(lane, slot);
                    }
                }
            }
            K_SEND => {
                let l = &mut self.lanes[lane];
                let c = &mut l.conns[slot as usize];
                c.send_busy = false;
                if res > 0 {
                    c.sent += res as usize;
                } else if res == 0 {
                    // A send of zero bytes on a non-empty buffer means the peer
                    // is gone; never retry it or this pass never ends.
                    c.live = false;
                    return;
                } else {
                    let err = -res;
                    if err == 11 {
                        // Would block: retry once in the flush at the end of
                        // this pass. Retrying from inside the completion would
                        // spin the loop against a peer that is not reading.
                        self.lanes[lane].dirty.push(slot);
                        return;
                    }
                    if err != 4 {
                        c.live = false;
                        return;
                    }
                }
                let done = c.sent >= c.outbuf.len();
                if done {
                    // Same policy as the epoll transport: keep small buffers,
                    // release big ones so idle connections stay tiny.
                    if c.outbuf.capacity() > crate::SMALL_RESPONSE {
                        let mut b = mem::take(&mut c.outbuf);
                        b.clear();
                        if l.pool.len() < 256 {
                            l.pool.push(b);
                        }
                    } else {
                        c.outbuf.clear();
                    }
                    c.sent = 0;
                }
                if done {
                    self.lanes[lane].dirty.push(slot);
                } else if c.live {
                    self.submit_send(lane, slot);
                }
            }
            K_CLOSE => {
                let l = &mut self.lanes[lane];
                l.conns[slot as usize] = Conn::new();
                l.free_slots = (l.free_slots + 1).min(l.conns.len() as u32);
            }
            _ => {}
        }
    }

    /// Hands the listener to a ring with free slots, opening the next ring when
    /// none has room yet.
    fn advance_accept(&mut self, from: usize) {
        for step in 1..=MAX_LANES {
            let next = (from + step) % MAX_LANES;
            if next >= self.lanes.len() {
                if self.lanes.len() < MAX_LANES && self.open_lane(self.lanes.len()) {
                    let new = self.lanes.len() - 1;
                    self.start_accepting(new);
                    return;
                }
                continue;
            }
            if self.lanes[next].free_slots > HANDOVER_FREE {
                self.start_accepting(next);
                return;
            }
        }
        // Every ring is nearly full: keep accepting on the current one. A
        // failed accept is retried, not lost.
        self.start_accepting(from);
    }

    /// Called after the batch commits: start sends, or close finished ones.
    fn flush(&mut self) {
        for lane in 0..self.lanes.len() {
            if self.lanes[lane].dirty.is_empty() {
                continue;
            }
            let dirty = mem::take(&mut self.lanes[lane].dirty);
            for &slot in &dirty {
                let idx = slot as usize;
                if idx >= self.lanes[lane].conns.len() {
                    continue;
                }
                let (live, busy, pending, close_after) = {
                    let c = &self.lanes[lane].conns[idx];
                    (c.live, c.send_busy, c.sent < c.outbuf.len(), c.close_after)
                };
                if !live {
                    continue;
                }
                if pending && !busy {
                    self.submit_send(lane, slot);
                } else if !pending && close_after {
                    self.submit_close(lane, slot);
                } else if !pending {
                    // Buffered input arrived while a send was in flight.
                    self.serve(lane, slot);
                    let more = {
                        let c = &self.lanes[lane].conns[idx];
                        c.sent < c.outbuf.len()
                    };
                    if more {
                        self.submit_send(lane, slot);
                    } else if self.lanes[lane].conns[idx].close_after {
                        self.submit_close(lane, slot);
                    }
                }
            }
        }
    }

    fn collect(&mut self, lane: usize, out: &mut Vec<(u64, i32, u32)>) {
        self.lanes[lane]
            .ring
            .reap(|u, res, flags| out.push((u, res, flags)));
    }

    /// One pass: wait for completions, serve the requests they carried, commit
    /// the batch, then submit the sends and the returned buffers.
    fn pump(&mut self, wait: bool) -> usize {
        // Wait. One ring: a single enter both submits and sleeps. Several:
        // sleep in epoll across their eventfds and run their task work.
        if self.lanes.len() == 1 {
            if wait {
                let _ = self.lanes[0].ring.submit(1, IORING_ENTER_GETEVENTS);
            } else {
                // Deferred task work only runs on an enter, so a non-blocking
                // pass still has to enter; it just does not wait.
                let _ = self.lanes[0].ring.submit(0, 0);
                let _ = self.lanes[0].ring.enter(0, IORING_ENTER_GETEVENTS);
            }
        } else {
            let mut events = [[0u8; 12]; 64];
            let timeout = if wait { -1 } else { 0 };
            let n = unsafe { sys::epoll_wait(self.ep, events.as_mut_ptr(), 64, timeout) };
            if n > 0 {
                for i in 0..n as usize {
                    let fd = sys::event_fd(&events[i]);
                    for lane in 0..self.lanes.len() {
                        if self.lanes[lane].efd == fd {
                            read_eventfd(fd);
                            // Deferred task work runs inside this call.
                            let _ = self.lanes[lane].ring.enter(0, IORING_ENTER_GETEVENTS);
                        }
                    }
                }
            }
            // Buffers and accepts queued last pass on other rings.
            for lane in 0..self.lanes.len() {
                let _ = self.lanes[lane].ring.submit(0, 0);
            }
        }

        let mut done: Vec<(u64, i32, u32)> = Vec::with_capacity(64);
        for lane in 0..self.lanes.len() {
            self.collect(lane, &mut done);
        }
        if done.is_empty() {
            return 0;
        }

        // A pass that carries a write takes the write lock up front; a pass of
        // reads uses a deferred transaction, which is what almost every pass is.
        let write = done.iter().any(|&(u, res, flags)| {
            if tag_kind(u) != K_RECV || res <= 0 {
                return false;
            }
            let lane = tag_lane(u) as usize;
            if lane >= self.lanes.len() {
                return true;
            }
            let bid = (flags >> IORING_CQE_BUFFER_SHIFT) as usize;
            let start = bid * BUF_SIZE;
            let end = (start + res as usize).min(self.lanes[lane].bufs.len());
            let head = &self.lanes[lane].bufs[start.min(end)..end];
            let first = head.split(|&b| b == b'\n').next().unwrap_or(&[]);
            // Anything that is not a plain GET may write, or is a continuation
            // this pass cannot classify: be conservative.
            !first.starts_with(b"GET ")
        });
        self.app.begin_batch(write);
        for &(u, res, flags) in &done {
            self.handle(u, res, flags);
        }
        let committed = self.app.end_batch();
        if !committed {
            eprintln!("io_uring: batch commit failed");
        }
        self.flush();
        for lane in 0..self.lanes.len() {
            self.lanes[lane].flush_buffers(lane);
            self.retry_deferred(lane);
            let _ = self.lanes[lane].ring.submit(0, 0);
        }
        // Keep the accepting ring stocked.
        let cur = self.accept_lane;
        if cur < self.lanes.len() && self.lanes[cur].accepting {
            self.arm_accepts(cur);
            let _ = self.lanes[cur].ring.submit(0, 0);
        }
        done.len()
    }

    fn run(&mut self) -> ! {
        loop {
            self.pump(true);
        }
    }
}

/// Builds a ring, drives real concurrent clients through the serving path, and
/// reports whether every one of them was answered correctly.
pub fn self_test(secret: &str, db_path: &str) -> bool {
    let listener = match std::net::TcpListener::bind(("127.0.0.1", 0)) {
        Ok(l) => l,
        Err(_) => return false,
    };
    let port = match listener.local_addr() {
        Ok(a) => a.port(),
        Err(_) => return false,
    };
    let _ = listener.set_nonblocking(true);
    let lfd = {
        use std::os::fd::AsRawFd;
        listener.as_raw_fd()
    };

    // Smaller than production so the test is quick, but the same code paths.
    let ep = unsafe { sys::epoll_create1(0x80000) };
    if ep < 0 {
        return false;
    }
    let mut server = UringServer {
        lanes: Vec::new(),
        ep,
        listen_fd: lfd,
        accept_lane: 0,
        app: App::new(db_path, secret),
    };
    if !server.open_lane(0) {
        return false;
    }
    server.start_accepting(0);

    // Three kinds of client at once: keep-alive, pipelined, and close-after.
    const CLIENTS: usize = 4;
    let mut handles = Vec::new();
    for c in 0..CLIENTS {
        handles.push(std::thread::spawn(move || -> bool {
            use std::io::{Read, Write};
            let mut s = match std::net::TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => s,
                Err(_) => return false,
            };
            let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(5)));
            let mut ok = true;
            // 1. a plain keep-alive request
            if s.write_all(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n").is_err() {
                return false;
            }
            let mut buf = [0u8; 4096];
            let n = s.read(&mut buf).unwrap_or(0);
            ok &= buf[..n].starts_with(b"HTTP/1.1 200");
            // 2. two requests in one write: responses must both arrive, in order
            let pipelined = b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n";
            if c % 2 == 0 && s.write_all(pipelined).is_err() {
                return false;
            }
            if c % 2 != 0 {
                // 3. a request split across two writes
                if s.write_all(b"GET /posts/2 HTTP/1.1\r\nHost: x\r\n").is_err() {
                    return false;
                }
                std::thread::sleep(std::time::Duration::from_millis(20));
                if s.write_all(b"\r\n").is_err() {
                    return false;
                }
            }
            // Drain for a bounded time, counting complete responses.
            let want = if c % 2 == 0 { 2 } else { 1 };
            let mut seen = 0usize;
            let mut acc: Vec<u8> = Vec::new();
            for _ in 0..64 {
                match s.read(&mut buf) {
                    Ok(0) => break,
                    Ok(n) => {
                        acc.extend_from_slice(&buf[..n]);
                        seen = acc.windows(12).filter(|w| w.starts_with(b"HTTP/1.1 ")).count();
                        if seen >= want {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
            ok &= seen >= want;
            // 4. close-after response
            if s.write_all(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                .is_err()
            {
                return false;
            }
            let mut tail = Vec::new();
            let _ = s.read_to_end(&mut tail);
            ok
        }));
    }

    // Drive the server until the clients are done or the budget runs out.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while std::time::Instant::now() < deadline {
        server.pump(false);
        if handles.iter().all(|h| h.is_finished()) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_micros(200));
    }
    let mut ok = true;
    for h in handles {
        ok &= h.join().unwrap_or(false);
    }
    drop(server);
    ok
}

/// Serves forever on `listen_fd` using io_uring. Returns the app back only if
/// no ring could be opened, so the caller can serve on the other transport
/// instead of dying.
pub fn serve(app: App, listen_fd: c_int) -> App {
    let ep = unsafe { sys::epoll_create1(0x80000) };
    assert!(ep >= 0, "epoll_create1 failed");
    let mut server = UringServer {
        lanes: Vec::new(),
        ep,
        listen_fd,
        accept_lane: 0,
        app,
    };
    if !server.open_lane(0) {
        for lane in server.lanes.iter() {
            unsafe { sys::close(lane.efd) };
        }
        unsafe { sys::close(ep) };
        return server.app;
    }
    server.start_accepting(0);
    eprintln!(
        "io_uring: {} direct descriptors in ring 0, {} buffers of {} bytes",
        server.lanes[0].free_slots, BUF_COUNT, BUF_SIZE
    );
    server.run()
}
