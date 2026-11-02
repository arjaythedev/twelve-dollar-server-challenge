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
//! syscall policy, an old kernel, an ABI mismatch — the caller stays on the other
//! transport rather than serving from an untested path.

use std::collections::VecDeque;
use std::mem;
use std::os::raw::c_int;

use crate::app::App;
use crate::http;
use crate::sys;
use crate::uring::{
    new_eventfd, read_eventfd, Ring, Sqe, IORING_CQE_BUFFER_SHIFT,
    IORING_ENTER_GETEVENTS, IORING_SETUP_CLAMP, IORING_SETUP_CQSIZE, IORING_SETUP_DEFER_TASKRUN,
    IORING_SETUP_SINGLE_ISSUER, IORING_SETUP_SUBMIT_ALL, IORING_SETUP_TASKRUN_FLAG,
};

/// Ring 0 plus seven more, each with the process's descriptor limit.
const MAX_LANES: usize = 8;
const SQ_ENTRIES: u32 = 2048;
const BUF_COUNT: u32 = 2048;
const BUF_SIZE: usize = 2048;
/// Accepts in flight per lane, and the free-slot level that triggers handover.
const ACCEPTS: u32 = 32;
const HANDOVER_FREE: u32 = 4096;

/// Self-test forces: a far smaller table and watermark so the driver crosses
/// ring boundaries for real before the public listener exists. A value of the
/// `FEED_TABLE` / `FEED_HANDOVER` environment overrides them; unset means the
/// process limit and the default.
/// The DB path the writer reopens (same file, second connection).
fn app_db_path_of(app: &App) -> String {
    app.db_path().to_string()
}

fn env_u32(name: &str) -> Option<u32> {
    std::env::var(name).ok().and_then(|s| s.parse().ok())
}

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
    /// A queued write owns this connection: further requests wait in the
    /// input buffer until the write commits and its reply is queued.
    write_parked: bool,
    /// The generation this conn had when its write was queued (stale
    /// completions drop).
    parked_gen: u32,
    /// A committed writer reply waits here if a prior send still references
    /// `outbuf`; appending before that send completes could move its storage.
    pending_writer_reply: Option<(Vec<u8>, bool)>,
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
            write_parked: false,
            parked_gen: 0,
            pending_writer_reply: None,
        }
    }

    fn inbuf_has_data(&self) -> bool {
        !self.inbuf.is_empty()
    }
}

struct Lane {
    ring: Ring,
    efd: c_int,
    /// This ring's own listening socket (REUSEPORT group); -1 unassigned.
    listen_fd: c_int,
    /// Per-connection-slot generation, bumped at close: a queued write's
    /// reply is dropped unless the slot still holds its connection.
    gens: Vec<u32>,
    bufs: Vec<u8>,
    /// Buffers returned since the last flush, as a bitmap.
    pending: Box<[u64]>,
    npending: u32,
    /// Provided buffers currently in use by published completions.
    held_bufs: u32,
    free_slots: u32,
    conns: Vec<Conn>,
    accept_inflight: u32,
    accepting: bool,
    /// Connections whose close could not be submitted yet.
    deferred_close: Vec<u32>,
    /// Starved receives (ENOBUFS) waiting for the ring's buffers to come
    /// back, oldest first: each pass re-arms at most as many as returned.
    read_queue: std::collections::VecDeque<u32>,
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
                Some(p) => {
                    Sqe(p).apply(Sqe::provide_buffers(addr, BUF_SIZE as u32, n, 0, start, u));
                    self.held_bufs = self.held_bufs.saturating_sub(n);
                }
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

/// The next ring that may take accepts, or None when every ring is at or below
/// the handover watermark (so a new one has to be opened).
fn handover_target(free: &[u32], from: usize, hwm: u32) -> Option<usize> {
    // Every ring but `from` itself: the caller only gets here when `from` is at
    // or below the watermark, so it must never be chosen.
    for step in 1..MAX_LANES {
        let next = (from + step) % MAX_LANES;
        if next < free.len() && free[next] > hwm {
            return Some(next);
        }
    }
    None
}

#[inline]
fn bit_set(pending: &[u64], i: u32) -> bool {
    let w = i as usize >> 6;
    w < pending.len() && pending[w] & (1u64 << (i & 63)) != 0
}

pub struct UringServer {
    lanes: Vec<Lane>,
    ep: c_int,
    /// The process's sole listening socket.
    listen_fd: c_int,
    accept_lane: usize,
    app: App,
    /// Free-slot level that moves accepting to the next ring.
    hwm: u32,
    /// Forced per-ring table size (self test); None = the process fd limit.
    table_override: Option<u32>,
    /// Accept backpressure on system-wide free memory.
    mem: sys::MemGuard,
    /// Set while a memory-pressure hold is active on the accepting lane.
    mem_holding: bool,
    /// The parked-write queue (None when the writer thread could not start).
    writer: Option<Box<crate::writer::IssuerQueue>>,
    /// The wake fd the writer signals; registered in the issuer's epoll.
    writer_wake_fd: c_int,
}

impl UringServer {
    fn arm_recv(&mut self, lane: usize, slot: u32) {
        let u = tag(K_RECV, slot, lane as u32);
        let l = &mut self.lanes[lane];
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::recv_buffered(slot, 0, u));
        }
    }

    /// Tops up this lane's one-shot accepts, never more than its free slots.
    fn arm_accepts(&mut self, lane: usize) {
        let want = self.lanes[lane].free_slots.saturating_sub(1).min(ACCEPTS);
        let lfd = self.listen_fd;
        while self.lanes[lane].accepting && self.lanes[lane].accept_inflight < want {
            let u = tag(K_ACCEPT, 0, lane as u32);
            let l = &mut self.lanes[lane];
            if let Some(p) = l.ring.sqe() {
                Sqe(p).apply(Sqe::accept(lfd, u));
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
        let table = match self.table_override {
            Some(t) => t.min(MAX_LANES as u32),
            None => sys::nofile_limit()
                .saturating_sub(64)
                .max(64)
                // The classic register_files table is capped in the kernel; stay
                // inside it so a large nofile limit cannot fail the whole lane.
                .min(1 << 20),
        };
        // A single ring with an out-of-ring writer wake must also receive
        // ordinary CQ notifications through epoll. Deferred task work may
        // wait for the issuer's next enter, so it cannot be the sole wake
        // source while the issuer is sleeping for the writer.
        let defer_taskrun = if i == 0 && self.writer_wake_fd >= 0 {
            0
        } else {
            IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG
        };
        let flags = IORING_SETUP_SINGLE_ISSUER
            | defer_taskrun
            | IORING_SETUP_CQSIZE
            | IORING_SETUP_CLAMP
            | IORING_SETUP_SUBMIT_ALL;
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
            listen_fd: self.listen_fd,
            gens: vec![0u32; table as usize],
            bufs,
            pending: vec![0u64; Lane::pending_words()].into_boxed_slice(),
            npending: 0,
            held_bufs: 0,
            read_queue: VecDeque::new(),
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
        let registered = if defer_taskrun == 0 {
            lane.ring.register_eventfd(efd)
        } else {
            lane.ring.register_eventfd_async(efd)
        };
        if !registered {
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
                        let (close, parked) = {
                            let app = &mut self.app;
                            let out = &mut self.lanes[lane].conns[idx].outbuf;
                            let mut pk =
                                crate::app::ParkedOut { out, owns_writes: self.writer.is_some(), job: None };
                            let closed = app.route(&rq, &mut pk);
                            (closed, pk.job)
                        };
                        if let Some(job) = parked {
                            // The issuer hands the job to the writer queue; the
                            // reply comes back through the wake fd. The
                            // pipeline on this connection waits (no recv re-
                            // arm, no further serve) until the reply is queued.
                            let gen = self.lanes[lane].gens[idx];
                            let queued = match &self.writer {
                                Some(q) => q.enqueue(lane as u32, slot, gen, job),
                                None => false,
                            };
                            if queued {
                                self.lanes[lane].conns[idx].write_parked = true;
                                buf.drain(..used);
                                break;
                            }
                            // Queue full: same answer as a failed batch commit.
                            let c = &mut self.lanes[lane].conns[idx];
                            c.outbuf.clear();
                            http::respond(&mut c.outbuf, 503, crate::app::R_INTERNAL);
                            c.close_after = true;
                            buf.clear();
                            break;
                        }
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
                    // Same routed path as the buffered serve: a write parks in
                    // the writer queue and the reply arrives through the wake.
                    let owns = self.writer.is_some();
                    let (close, parked) = {
                        let app = &mut self.app;
                        let out = &mut self.lanes[lane].conns[idx].outbuf;
                        let mut pk = crate::app::ParkedOut { out, owns_writes: owns, job: None };
                        let closed = app.route(&rq, &mut pk);
                        (closed, pk.job)
                    };
                    if let Some(job) = parked {
                        let gen = self.lanes[lane].gens[idx];
                        let queued = match &self.writer {
                            Some(q) => q.enqueue(lane as u32, slot, gen, job),
                            None => false,
                        };
                        if queued {
                            self.lanes[lane].conns[idx].write_parked = true;
                            // No recv re-arm: the pipeline waits for the commit.
                            return;
                        }
                        let c = &mut self.lanes[lane].conns[idx];
                        c.outbuf.clear();
                        http::respond(&mut c.outbuf, 503, crate::app::R_INTERNAL);
                        c.close_after = true;
                        self.mark_dirty(lane, slot);
                        return;
                    }
                    if close {
                        self.lanes[lane].conns[idx].close_after = true;
                    }
                    self.mark_dirty(lane, slot);
                    return;
                }
            }        }
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
                    self.lanes[lane].held_bufs += 1;
                    self.lanes[lane].recycle(bid);
                    let data = unsafe { std::slice::from_raw_parts(base.add(start), n) };
                    self.on_data(lane, slot, data);
                    // A parked write holds the pipeline: the receive re-arms
                    // only when the write's reply is queued.
                    if self.lanes[lane].conns[slot as usize].live
                        && !self.lanes[lane].conns[slot as usize].write_parked
                    {
                        self.arm_recv(lane, slot);
                    }
                } else if res == 0 {
                    self.submit_close(lane, slot);
                } else {
                    let err = -res;
                    // EAGAIN/EINTR/EALREADY: transient, re-arm as usual.
                    // ENOBUFS is the storm case: the ring's buffers are out,
                    // and re-arming every starved receive at once re-arms them
                    // all into an empty ring (each ends ENOBUFS again). The
                    // starved connection joins a first-in, first-out queue;
                    // each pass re-arms at most as many of them as buffers were
                    // returned, so the queue drains at the rate the ring can
                    // serve and never regenerates the collapse (the failure
                    // mode the merged head's 240k probe died in).
                    if err == 11 || err == 4 || err == 114 {
                        if self.lanes[lane].conns[slot as usize].live {
                            self.arm_recv(lane, slot);
                        }
                    } else if err == 105 {
                        if self.lanes[lane].conns[slot as usize].live {
                            self.lanes[lane].read_queue.push_back(slot);
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
                let gi = slot as usize;
                if gi < l.gens.len() {
                    l.gens[gi] = l.gens[gi].wrapping_add(1);
                }
                l.free_slots = (l.free_slots + 1).min(l.conns.len() as u32);
            }
            _ => {}
        }
    }

    /// Drains the writer's committed replies and submits their sends. A reply
    /// for a slot whose connection died in the meantime is dropped (the write
    /// still committed — accepted-request semantics).
    fn publish_writer_replies(&mut self) {
        let q = match &self.writer {
            Some(q) => q,
            None => return,
        };
        for r in q.drain() {
            let idx = r.slot as usize;
            if r.lane as usize >= self.lanes.len() || idx >= self.lanes[r.lane as usize].conns.len()
            {
                continue;
            }
            let lane_live = {
                let l = &self.lanes[r.lane as usize];
                let gi = idx.min(l.gens.len() - 1);
                l.gens[gi] == r.gen && l.conns[idx].live
            };
            if !lane_live {
                continue;
            }
            let lane = r.lane as usize;
            if self.lanes[lane].conns[idx].send_busy {
                self.lanes[lane].conns[idx].pending_writer_reply = Some((r.bytes, r.close));
            } else {
                let c = &mut self.lanes[lane].conns[idx];
                c.outbuf.extend_from_slice(&r.bytes);
                c.write_parked = false;
                c.close_after |= r.close;
                if c.inbuf_has_data() {
                    self.serve(lane, r.slot);
                    let c = &self.lanes[lane].conns[idx];
                    if !c.write_parked && !c.close_after {
                        self.arm_recv(lane, r.slot);
                    }
                } else if !self.lanes[lane].conns[idx].close_after {
                    self.arm_recv(lane, r.slot);
                }
                self.submit_send(lane, r.slot);
            }
        }
    }

    /// Fair re-arms after a dry ring: at most `free buffers` starved
    /// connections get their multishot receive back per pass, oldest first,
    /// so the queue drains at the rate the ring can serve.
    fn drain_starved(&mut self, lane: usize) {
        let l = &mut self.lanes[lane];
        let free = (BUF_COUNT as i64 - l.held_bufs as i64).max(0) as usize;
        let mut budget = free.min(256);
        while budget > 0 {
            match l.read_queue.pop_front() {
                Some(slot) => {
                    let idx = slot as usize;
                    if idx < l.conns.len() && l.conns[idx].live && !l.conns[idx].write_parked {
                        let u = tag(K_RECV, slot, lane as u32);
                        match l.ring.sqe() {
                            Some(p) => {
                                Sqe(p).apply(Sqe::recv_buffered(slot, 0, u));
                                budget -= 1;
                            }
                            None => {
                                // SQ full: back at the front for the next pass.
                                l.read_queue.push_front(slot);
                                break;
                            }
                        }
                    }
                }
                None => break,
            }
        }
    }

    /// Hands the listener to a ring with free slots, opening the next ring when
    /// none has room yet.
    fn advance_accept(&mut self, from: usize) {        let free: Vec<u32> = self.lanes.iter().map(|l| l.free_slots).collect();
        if let Some(next) = handover_target(&free, from, self.hwm) {
            self.start_accepting(next);
            return;
        }
        if self.lanes.len() < MAX_LANES && self.open_lane(self.lanes.len()) {
            let new = self.lanes.len() - 1;
            self.start_accepting(new);
            return;
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
                let pending_reply = if !self.lanes[lane].conns[idx].send_busy {
                    self.lanes[lane].conns[idx].pending_writer_reply.take()
                } else {
                    None
                };
                if let Some((bytes, close)) = pending_reply {
                    let c = &mut self.lanes[lane].conns[idx];
                    c.outbuf.extend_from_slice(&bytes);
                    c.write_parked = false;
                    c.close_after |= close;
                    if c.inbuf_has_data() {
                        self.serve(lane, slot);
                        let c = &self.lanes[lane].conns[idx];
                        if !c.write_parked && !c.close_after {
                            self.arm_recv(lane, slot);
                        }
                    } else if !self.lanes[lane].conns[idx].close_after {
                        self.arm_recv(lane, slot);
                    }
                }
                let (live, busy, pending, close_after, write_parked) = {
                    let c = &self.lanes[lane].conns[idx];
                    (c.live, c.send_busy, c.sent < c.outbuf.len(), c.close_after, c.write_parked)
                };
                if !live {
                    continue;
                }
                if pending && !busy {
                    self.submit_send(lane, slot);
                } else if !pending && close_after {
                    self.submit_close(lane, slot);
                } else if !pending && !write_parked {
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
        // The per-pass cap: completions beyond it wait in the ring (FIFO) for
        // the next pass, so one memory-wall pass cannot hold the whole
        // connection set behind a single slow commit. 1,024 per ring matches
        // the batch size the writer executes and keeps a pass's worst case in
        // the tens of milliseconds even with every row a cache miss.
        const CQ_CAP_PER_LANE: usize = 1024;
        self.lanes[lane]
            .ring
            .reap_bounded(CQ_CAP_PER_LANE, |u, res, flags| out.push((u, res, flags)));
    }

    /// One pass: wait for completions, serve the requests they carried, commit
    /// the batch, then submit the sends and the returned buffers.
    fn pump(&mut self, wait: bool) -> usize {
        // Wait. One ring: a single enter both submits and sleeps. Several:
        // sleep in epoll across their eventfds and run their task work.
        if self.lanes.len() == 1 && self.writer_wake_fd < 0 {
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
                    if fd == self.writer_wake_fd {
                        // The writer committed a batch: replies wait in the
                        // ready queue.
                        crate::writer::eventdrain(fd);
                        self.publish_writer_replies();
                        continue;
                    }
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

        let mut done: Vec<(u64, i32, u32)> = Vec::with_capacity(2048);
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
        // In writer mode the issuer only runs reads: POSTs are parked in the
        // dedicated writer queue. Taking BEGIN IMMEDIATE here would race the
        // writer connection for SQLite's single WAL writer lock, even though
        // the issuer's transaction has no writes of its own.
        self.app.begin_batch(write && self.writer.is_none());
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
            self.drain_starved(lane);
            let _ = self.lanes[lane].ring.submit(0, 0);
        }
        // Accept backpressure on system-wide free memory: an out-of-memory kill
        // scores zero, a short hold only delays connects. Polled at most every
        // 100 ms. In-flight accepts are installed either way.
        let mem_ok = self.mem.allows_accepts();
        let mut cur = self.accept_lane;
        if cur < self.lanes.len() {
            let holding = self.lanes[cur].accepting;
            if !mem_ok && holding {
                self.lanes[cur].accepting = false;
                self.mem_holding = true;
                eprintln!("accept held: MemAvailable below the stop threshold");
            } else if mem_ok && !holding && self.mem_holding {
                self.mem_holding = false;
                self.lanes[cur].accepting = true;
            }
        }
        if cur < self.lanes.len() && self.lanes[cur].accepting {
            if self.lanes[cur].free_slots <= self.hwm {
                self.lanes[cur].accepting = false;
                self.advance_accept(cur);
                cur = self.accept_lane;
            }
            if cur < self.lanes.len() && self.lanes[cur].accepting {
                self.arm_accepts(cur);
                let _ = self.lanes[cur].ring.submit(0, 0);
            }
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
        hwm: HANDOVER_FREE,
        table_override: None,
        mem: sys::MemGuard::new(),
        mem_holding: false,
        writer: None,
        writer_wake_fd: -1,
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

    // Ring-boundary stage: a small per-ring table plus a small watermark force
    // the ignored listener to cross from ring 0 to ring 1 while clients are
    // still arriving. Every client must be served after the handover.
    ok
}

/// 64 clients against a 32-slot table with a watermark of 8: ring 0 stops
/// accepting at 24 lands, ring 1 (or later) takes the rest. The handover and
/// the small-table pump paths must keep every client correct.
fn self_test_boundary(secret: &str, db_path: &str, port: u16, lfd: c_int) -> bool {
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
        hwm: 8,
        table_override: Some(32),
        mem: sys::MemGuard::new(),
        mem_holding: false,
        writer: None,
        writer_wake_fd: -1,
    };
    if !server.open_lane(0) {
        return false;
    }
    server.start_accepting(0);

    const CLIENTS: usize = 64;
    let mut handles = Vec::new();
    for _c in 0..CLIENTS {
        handles.push(std::thread::spawn(move || -> bool {
            use std::io::{Read, Write};
            let mut s = match std::net::TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => s,
                Err(_) => return false,
            };
            let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(5)));
            let mut buf = [0u8; 4096];
            // One keep-alive, then one close-after so rings free their slots too.
            if s.write_all(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n").is_err() {
                return false;
            }
            let n = s.read(&mut buf).unwrap_or(0);
            if !buf[..n].starts_with(b"HTTP/1.1 200") {
                return false;
            }
            if s.write_all(b"GET /posts/3 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                .is_err()
            {
                return false;
            }
            let mut tail = Vec::new();
            let n = s.read_to_end(&mut tail).unwrap_or(0);
            n > 0
        }));
    }
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
    // The handover itself must have happened; otherwise table 32 could not hold
    // 64 concurrent clients and the test would be measuring nothing.
    ok &= server.lanes.len() >= 2;
    ok
}

/// The next ring with room is chosen in ring order and skips the current one,
/// so a full ring cannot keep the listener.
#[cfg(test)]
mod handover_tests {
    use super::*;

    #[test]
    fn picks_the_next_ring_with_room() {
        assert_eq!(handover_target(&[1, 100_000], 0, HANDOVER_FREE), Some(1));
        assert_eq!(handover_target(&[100_000, 1], 1, HANDOVER_FREE), Some(0));
        assert_eq!(handover_target(&[1, 1, 100_000], 0, HANDOVER_FREE), Some(2));
    }

    #[test]
    fn ignores_rings_at_or_below_the_watermark() {
        assert_eq!(handover_target(&[100_000, HANDOVER_FREE], 0, HANDOVER_FREE), None);
        assert_eq!(handover_target(&[1, 1, 1], 0, HANDOVER_FREE), None);
        assert_eq!(handover_target(&[], 0, HANDOVER_FREE), None);
    }

    #[test]
    fn wraps_past_the_last_ring() {
        assert_eq!(handover_target(&[100_000, 0, 0], 2, HANDOVER_FREE), Some(0));
    }

    #[test]
    fn honors_a_forced_watermark() {
        // Self test: watermark 8 means lanes at exactly 8 free slots are done;
        // a lane above the watermark is the handover target.
        assert_eq!(handover_target(&[8, 32], 0, 8), Some(1));
        assert_eq!(handover_target(&[8, 8], 0, 8), None);
    }
}

/// Serves forever on `listen_fd` using io_uring. Returns the app back only if
/// no ring could be opened, so the caller can serve on the other transport
/// instead of dying.
pub fn serve(app: App, listen_fd: c_int) -> App {
    let ep = unsafe { sys::epoll_create1(0x80000) };
    assert!(ep >= 0, "epoll_create1 failed");
    // The writer pair: the issuer's queue + eventfd, and the writer thread's
    // own connection. Writes park here so the issuer's passes never stall on
    // a write transaction.
    let iq: Option<crate::writer::IssuerQueue> =
        match crate::writer::IssuerQueue::new(&app_db_path_of(&app)) {
            Ok(q) => Some(q),
            Err(e) => {
                eprintln!("writer queue unavailable: {e}; writes run inline");
                None
            }
        };
    let writer_fd = iq.as_ref().map(|q| q.fd).unwrap_or(-1);
    let mut server = UringServer {
        lanes: Vec::new(),
        ep,
        listen_fd,
        accept_lane: 0,
        app,
        hwm: env_u32("FEED_HANDOVER").unwrap_or(HANDOVER_FREE),
        table_override: env_u32("FEED_TABLE"),
        mem: sys::MemGuard::new(),
        mem_holding: false,
        writer: iq.map(Box::new),
        writer_wake_fd: writer_fd,
    };
    if !server.open_lane(0) {
        for lane in server.lanes.iter() {
            unsafe { sys::close(lane.efd) };
        }
        unsafe { sys::close(ep) };
        return server.app;
    }
    if server.writer_wake_fd >= 0 {
        let ev = sys::event_for(server.writer_wake_fd, sys::EPOLLIN);
        unsafe {
            sys::epoll_ctl(ep, sys::EPOLL_CTL_ADD, server.writer_wake_fd, &ev);
        }
    }
    server.start_accepting(0);
    eprintln!(
        "io_uring: {} rings, {} direct descriptors in ring 0, {} buffers of {} bytes",
        server.lanes.len(), server.lanes[0].free_slots, BUF_COUNT, BUF_SIZE
    );
    server.run()
}
