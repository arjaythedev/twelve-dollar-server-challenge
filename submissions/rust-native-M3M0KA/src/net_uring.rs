//! io_uring transport: one issuer thread, several rings of direct descriptors.
//!
//! Accepted sockets are installed in a ring's fixed-file table instead of the
//! process descriptor table, so `LimitNOFILE=65535` bounds one ring rather than
//! the whole server. Accept is multishot; each connection holds one multishot
//! receive into a provided-buffer ring, so an idle connection owns no user-space
//! buffer at all.
//!
//! `self_test` drives a real request through this exact code path before the
//! public listener exists. If anything fails — seccomp, an old kernel, an ABI
//! mismatch — the caller stays on the epoll transport.

use std::mem;
use std::os::raw::{c_int, c_void};

use crate::app::App;
use crate::http;
use crate::sys;
use crate::uring::{
    self, new_eventfd, read_eventfd, BufEntry, Ring, Sqe, IORING_CQE_F_MORE,
    IORING_CQE_BUFFER_SHIFT, IORING_ENTER_GETEVENTS, IORING_SETUP_COOP_TASKRUN,
    IORING_SETUP_SINGLE_ISSUER,
};

const RING_COUNT: usize = 6;
const SLOTS: u32 = 65535;
const SQ_ENTRIES: u32 = 4096;
const BUF_COUNT: u32 = 4096;
const BUF_SIZE: usize = 2048;
/// Hand accept to the next ring before this one runs out of slots.
const HANDOVER_FREE: u32 = 4096;

const K_ACCEPT: u64 = 1;
const K_RECV: u64 = 2;
const K_SEND: u64 = 3;
const K_SHUTDOWN: u64 = 4;
const K_CLOSE: u64 = 5;

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
    buf_ring: Box<[BufEntry]>,
    buf_tail: u32,
    free_slots: u32,
    conns: Vec<Conn>,
    accepting: bool,
    /// Connections with output ready to send, or waiting to be closed.
    dirty: Vec<u32>,
}

impl Lane {
    fn publish_tail(&mut self) {
        // `struct io_uring_buf_ring` overlays its 16-byte header on entry 0:
        // resv1 (8) + resv2 (4) + resv3 (2) + tail (2).
        unsafe {
            std::ptr::write_volatile(
                (self.buf_ring.as_mut_ptr() as *mut u8).add(14) as *mut u16,
                self.buf_tail as u16,
            );
        }
    }

    fn publish_all_buffers(&mut self) {
        for bid in 0..BUF_COUNT {
            let idx = bid as usize;
            let start = idx * BUF_SIZE;
            self.buf_ring[idx] = BufEntry {
                addr: unsafe { self.bufs.as_ptr().add(start) } as u64,
                len: BUF_SIZE as u32,
                bid: bid as u16,
                resv: 0,
            };
        }
        self.buf_tail = BUF_COUNT;
        self.publish_tail();
    }

    fn recycle(&mut self, bid: u32) {
        let idx = (self.buf_tail as usize) & (BUF_COUNT as usize - 1);
        let start = bid as usize * BUF_SIZE;
        self.buf_ring[idx] = BufEntry {
            addr: unsafe { self.bufs.as_ptr().add(start) } as u64,
            len: BUF_SIZE as u32,
            bid: bid as u16,
            resv: 0,
        };
        self.buf_tail = self.buf_tail.wrapping_add(1);
        self.publish_tail();
    }
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
            Sqe(p).apply(Sqe::recv_multishot(slot, lane as u16, u));
        }
    }

    fn arm_accept(&mut self, lane: usize) {
        let u = tag(K_ACCEPT, 0, lane as u32);
        let l = &mut self.lanes[lane];
        l.accepting = true;
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::accept(self.listen_fd, u));
        }
    }

    fn submit_close(&mut self, lane: usize, slot: u32) {
        let l = &mut self.lanes[lane];
        if l.conns[slot as usize].outbuf.is_empty()
            && l.conns[slot as usize].sent == 0
        {
            // Nothing buffered: a plain close is enough.
            if let Some(p) = l.ring.sqe() {
                Sqe(p).apply(Sqe::close(slot, tag(K_CLOSE, slot, lane as u32)));
            }
        } else if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::shutdown(slot, tag(K_SHUTDOWN, slot, lane as u32)));
        }
        if let Some(p) = l.ring.sqe() {
            Sqe(p).apply(Sqe::close(slot, tag(K_CLOSE, slot, lane as u32)));
        }
        l.conns[slot as usize].live = false;
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
        if !buf.is_empty() {
            self.lanes[lane].conns[idx].inbuf = buf;
        }
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
                if res >= 0 {
                    let new_slot = res as u32;
                    let l = &mut self.lanes[lane];
                    l.conns[new_slot as usize] = Conn::new();
                    l.conns[new_slot as usize].live = true;
                    l.free_slots = l.free_slots.saturating_sub(1);
                    self.arm_recv(lane, new_slot);
                } else {
                    let err = -res;
                    self.lanes[lane].accepting = false;
                    // ENFILE/EMFILE/ENOBUFS: this ring's table is full, so hand
                    // accepting to the next ring. EINTR/ECANCELED: just re-arm.
                    if err == 23 || err == 24 || err == 105 {
                        self.advance_accept(lane);
                    } else if err == 4 || err == 125 || err == 11 {
                        self.arm_accept(lane);
                    } else {
                        self.advance_accept(lane);
                    }
                }
            }
            K_RECV => {
                if res > 0 {
                    let bid = flags >> IORING_CQE_BUFFER_SHIFT;
                    let start = bid as usize * BUF_SIZE;
                    let data = self.lanes[lane].bufs[start..start + BUF_SIZE].to_vec();
                    self.lanes[lane].recycle(bid);
                    self.on_data(lane, slot, &data[..res as usize]);
                    if flags & IORING_CQE_F_MORE == 0 && self.lanes[lane].conns[slot as usize].live {
                        self.arm_recv(lane, slot);
                    }
                } else if res == 0 {
                    self.submit_close(lane, slot);
                } else {
                    let err = -res;
                    if err == 105 || err == 11 || err == 4 || err == 125 {
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
                } else if res < 0 {
                    let err = -res;
                    if err != 11 && err != 4 {
                        c.live = false;
                        return;
                    }
                }
                let done = c.sent >= c.outbuf.len();
                if done {
                    c.outbuf = Vec::new();
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
                l.free_slots = (l.free_slots + 1).min(SLOTS);
            }
            _ => {}
        }
    }

    fn advance_accept(&mut self, from: usize) {
        for step in 1..=self.lanes.len() {
            let next = (from + step) % self.lanes.len();
            if self.lanes[next].free_slots > HANDOVER_FREE {
                self.accept_lane = next;
                self.arm_accept(next);
                return;
            }
        }
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

    fn reap(&mut self, lane: usize) {
        let mut done: Vec<(u64, i32, u32)> = Vec::new();
        self.lanes[lane].ring.reap(|u, res, flags| done.push((u, res, flags)));
        for (u, res, flags) in done {
            self.handle(u, res, flags);
        }
    }
}

impl Sqe {
    fn apply<F: FnOnce(&Sqe)>(self, f: F) {
        f(&self);
    }
}

/// Builds a small ring and drives one real request through it.
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

    let mut ring = match Ring::new(64, IORING_SETUP_COOP_TASKRUN) {
        Some(r) => r,
        None => return false,
    };
    let files = vec![-1 as c_int; 64];
    if !ring.register_files(&files) {
        return false;
    }
    let mut bufs = vec![0u8; 32 * 1024];
    let mut entries: Box<[BufEntry]> = vec![BufEntry::default(); 32].into_boxed_slice();
    for (i, e) in entries.iter_mut().enumerate() {
        *e = BufEntry {
            addr: unsafe { bufs.as_ptr().add(i * 1024) } as u64,
            len: 1024,
            bid: i as u16,
            resv: 0,
        };
    }
    if !ring.register_pbuf_ring(entries.as_mut_ptr() as *mut c_void, 32, 0) {
        return false;
    }
    unsafe {
        std::ptr::write_volatile((entries.as_mut_ptr() as *mut u8).add(14) as *mut u16, 32);
    }

    let client = std::thread::spawn(move || {
        use std::io::{Read, Write};
        let mut s = match std::net::TcpStream::connect(("127.0.0.1", port)) {
            Ok(s) => s,
            Err(_) => return false,
        };
        let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(5)));
        if s.write_all(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            .is_err()
        {
            return false;
        }
        let mut buf = [0u8; 512];
        let n = s.read(&mut buf).unwrap_or(0);
        buf[..n].starts_with(b"HTTP/1.1 200")
    });

    if let Some(p) = ring.sqe() {
        Sqe(p).apply(Sqe::accept(lfd, tag(K_ACCEPT, 0, 0)));
    }
    if ring.submit(0, 0) < 0 {
        return false;
    }

    let mut app = App::new(db_path, secret);
    let mut slot: i32 = -1;
    let mut answered = false;
    let mut keepalive: Vec<Vec<u8>> = Vec::new();
    for _ in 0..400 {
        let _ = ring.enter(1, IORING_ENTER_GETEVENTS);
        let mut done: Vec<(u64, i32, u32)> = Vec::new();
        ring.reap(|u, res, flags| done.push((u, res, flags)));
        for (u, res, flags) in done {
            match tag_kind(u) {
                K_ACCEPT if res >= 0 => {
                    slot = res;
                    if let Some(p) = ring.sqe() {
                        Sqe(p).apply(Sqe::recv_multishot(
                            slot as u32,
                            0,
                            tag(K_RECV, slot as u32, 0),
                        ));
                    }
                }
                K_RECV if res > 0 => {
                    let bid = (flags >> IORING_CQE_BUFFER_SHIFT) as usize;
                    let start = bid * 1024;
                    let data = bufs[start..start + res as usize].to_vec();
                    let mut out = Vec::new();
                    if let Ok(Some((rq, _))) = http::parse(&data) {
                        app.handle(&rq, &mut out);
                    }
                    let _ = app.end_batch();
                    if !out.is_empty() {
                        answered = true;
                        keepalive.push(out);
                        let b = keepalive.last().unwrap();
                        if let Some(p) = ring.sqe() {
                            Sqe(p).apply(Sqe::send(
                                slot as u32,
                                b.as_ptr(),
                                b.len(),
                                tag(K_SEND, slot as u32, 0),
                            ));
                        }
                    }
                }
                K_SEND => {
                    if let Some(p) = ring.sqe() {
                        Sqe(p).apply(Sqe::close(slot as u32, tag(K_CLOSE, slot as u32, 0)));
                    }
                }
                _ => {}
            }
        }
        let _ = ring.submit(0, 0);
        if answered && ring.pending() == 0 && slot >= 0 {
            // One more pass to let CLOSE complete.
            let _ = ring.enter(1, IORING_ENTER_GETEVENTS);
            let _ = ring.reap(|_, _, _| {});
            break;
        }
    }
    let ok = answered && client.join().unwrap_or(false);
    drop(app);
    drop(keepalive);
    ok
}

/// Serves forever on `listen_fd` using io_uring.
pub fn serve(app: App, listen_fd: c_int) -> ! {
    let ep = unsafe { sys::epoll_create1(0x80000) };
    assert!(ep >= 0, "epoll_create1 failed");

    let mut lanes: Vec<Lane> = Vec::with_capacity(RING_COUNT);
    for i in 0..RING_COUNT {
        let ring = match Ring::new(SQ_ENTRIES, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN)
        {
            Some(r) => r,
            None => {
                eprintln!("io_uring: ring {i} unavailable");
                std::process::exit(1);
            }
        };
        if !ring.register_files(&vec![-1 as c_int; SLOTS as usize]) {
            eprintln!("io_uring: cannot register fixed files");
            std::process::exit(1);
        }
        let bufs = vec![0u8; BUF_COUNT as usize * BUF_SIZE];
        let buf_ring: Box<[BufEntry]> =
            vec![BufEntry::default(); BUF_COUNT as usize].into_boxed_slice();
        let mut lane = Lane {
            ring,
            efd: -1,
            bufs,
            buf_ring,
            buf_tail: 0,
            free_slots: SLOTS,
            conns: (0..SLOTS).map(|_| Conn::new()).collect(),
            accepting: false,
            dirty: Vec::with_capacity(1024),
        };
        if !lane
            .ring
            .register_pbuf_ring(lane.buf_ring.as_mut_ptr() as *mut c_void, BUF_COUNT, i as u16)
        {
            eprintln!("io_uring: provided buffer ring refused");
            std::process::exit(1);
        }
        lane.publish_all_buffers();
        let efd = new_eventfd();
        if efd < 0 || !lane.ring.register_eventfd_async(efd) {
            eprintln!("io_uring: eventfd registration refused");
            std::process::exit(1);
        }
        lane.efd = efd;
        let ev = sys::event_for(efd, sys::EPOLLIN);
        unsafe {
            sys::epoll_ctl(ep, sys::EPOLL_CTL_ADD, efd, &ev);
        }
        lanes.push(lane);
    }

    let mut server = UringServer {
        lanes,
        ep,
        listen_fd,
        accept_lane: 0,
        app,
    };
    server.arm_accept(0);
    let _ = server.lanes[0].ring.submit(0, 0);
    eprintln!(
        "io_uring: {} rings of {} direct descriptors, {} buffers each",
        RING_COUNT, SLOTS, BUF_COUNT
    );

    let mut events = [[0u8; 12]; 64];
    loop {
        server.app.begin_batch(true);
        let n = unsafe { sys::epoll_wait(server.ep, events.as_mut_ptr(), 64, -1) };
        if n < 0 {
            if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            panic!("epoll_wait: {}", std::io::Error::last_os_error());
        }
        for i in 0..n as usize {
            let fd = sys::event_fd(&events[i]);
            for lane in 0..server.lanes.len() {
                if server.lanes[lane].efd == fd {
                    read_eventfd(fd);
                    server.reap(lane);
                }
            }
        }
        for lane in 0..server.lanes.len() {
            if server.lanes[lane].ring.pending() > 0 {
                server.reap(lane);
            }
        }
        let committed = server.app.end_batch();
        if !committed {
            eprintln!("io_uring: batch commit failed");
        }
        server.flush();
        for lane in 0..server.lanes.len() {
            let _ = server.lanes[lane].ring.submit(0, 0);
        }
        let cur = server.accept_lane;
        if !server.lanes[cur].accepting && server.lanes[cur].free_slots > HANDOVER_FREE {
            server.arm_accept(cur);
            let _ = server.lanes[cur].ring.submit(0, 0);
        }
    }
}
