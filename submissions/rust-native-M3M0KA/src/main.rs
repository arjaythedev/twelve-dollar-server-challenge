//! $12 server challenge entry: a native Rust HTTP/1.1 feed server.
//!
//! One thread, one SQLite connection, no framework, no third-party crates.
//! Configuration comes only from SQLITE_PATH, JWT_SECRET, HOST and PORT.
//!
//! One pass of the event loop is one SQLite transaction. Responses are built
//! into owned provisional connection buffers and sent only after commit returns.
//! A connection executes at most one request until its previous reply drains.

mod app;
mod buffer;
mod btree;
mod crypto;
mod db;
mod http;
mod l2_server;
mod json;
mod l2_uring;
mod net_uring;
mod sys;
mod uring;

use std::io::Write;
use std::collections::VecDeque;
use std::mem;
use std::os::raw::{c_int, c_void};
use std::time::{Duration, Instant};

use app::App;

/// Response buffers larger than this are released instead of retained per
/// connection. Everything except `/feed` fits comfortably below it.
pub const SMALL_RESPONSE: usize = 2048;

/// Retain a small, bounded set of completed request buffers for reuse.
const INPUT_POOL_MAX_BUFFERS: usize = 256;
const INPUT_POOL_MAX_BUFFER_BYTES: usize = 32 * 1024;
const INPUT_POOL_MAX_BYTES: usize = 1024 * 1024;

#[inline]
fn input_pool_buffer_allowed(count: usize, bytes: usize, capacity: usize) -> bool {
    capacity != 0
        && capacity <= INPUT_POOL_MAX_BUFFER_BYTES
        && count < INPUT_POOL_MAX_BUFFERS
        && bytes <= INPUT_POOL_MAX_BYTES.saturating_sub(capacity)
}

struct Conn {
    live: bool,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    sent: usize,
    close_after: bool,
    want_out: bool,
    read_eof: bool,
    continue_sent: bool,
    queued: bool,
    generation: u64,
}

impl Conn {
    fn new() -> Conn {
        Conn {
            live: false,
            inbuf: Vec::new(),
            outbuf: Vec::new(),
            sent: 0,
            close_after: false,
            want_out: false,
            read_eof: false,
            continue_sent: false,
            queued: false,
            generation: 0,
        }
    }
}

struct Pending {
    fd: c_int,
    close_after: bool,
}

struct Server {
    ep: c_int,
    conns: Vec<Conn>,
    app: App,
    pending: Vec<Pending>,
    /// Tracks actual listener registration. Resource holds retry independently
    /// so a worker can recover after descriptors or memory become available.
    accepting: bool,
    accept_retry_at: Option<Instant>,
    /// Accept backpressure on system-wide free memory: with "MemAvailable"
    /// below the stop threshold this worker leaves the epoll set like an
    /// EMFILE'd one, and rejoins above the resume threshold.
    mem: sys::MemGuard,
    /// Response buffers taken back from connections after their send completed.
    /// A feed response is a few KB; keeping one per connection would cost
    /// gigabytes at 300k sockets, and freeing one per request costs syscalls.
    pool: Vec<Vec<u8>>,
    input_pool_enabled: bool,
    input_pool: Vec<Vec<u8>>,
    input_pool_bytes: usize,
    output_bytes: usize,
    pool_bytes: usize,
    ready: VecDeque<(c_int, u64)>,
    next_generation: u64,
}

const POOL_MAX: usize = 256;
const OUTPUT_MAX_BYTES: usize = 64 * 1024 * 1024;
const OUTPUT_POOL_MAX_BYTES: usize = 1024 * 1024;
const OUTPUT_POOL_MAX_BUFFER: usize = 64 * 1024;
const REQUESTS_PER_PASS: usize = 256;
const ERROR_RESERVE: usize = 512;

impl Server {
    /// Resource suspension must recover even when no clients produce events.
    /// Listener EPOLLEXCLUSIVE registrations require DEL/ADD rather than MOD.
    fn set_accepting(&mut self, lfd: c_int, enabled: bool) {
        if self.accepting == enabled {
            return;
        }
        let ev = sys::event_for(lfd, sys::EPOLL_IN_FOR_LISTENER);
        let op = if enabled { sys::EPOLL_CTL_ADD } else { sys::EPOLL_CTL_DEL };
        let rc = unsafe { sys::epoll_ctl(self.ep, op, lfd, &ev) };
        if rc < 0 {
            // Continuing with the wrong registration can spin or permanently
            // strand the listener. Surface the actual failure immediately.
            panic!("listener epoll_ctl: {}", std::io::Error::last_os_error());
        }
        self.accepting = enabled;
    }

    fn refresh_accepting(&mut self, lfd: c_int) {
        let resource_ready = self.accept_retry_at.map_or(true, |at| Instant::now() >= at);
        let memory_ready = self.mem.allows_accepts();
        if resource_ready {
            self.accept_retry_at = None;
        }
        self.set_accepting(lfd, resource_ready && memory_ready);
    }

    fn touch(&mut self, fd: usize) -> bool {
        if fd >= self.conns.len() {
            if self.conns.try_reserve(fd + 1 - self.conns.len()).is_err() { return false; }
            self.conns.resize_with(fd + 1, Conn::new);
        }
        true
    }

    fn arm(&self, fd: c_int, events: u32) {
        let ev = sys::event_for(fd, events);
        let rc = unsafe { sys::epoll_ctl(self.ep, sys::EPOLL_CTL_MOD, fd, &ev) };
        assert!(rc >= 0, "client epoll_ctl: {}", std::io::Error::last_os_error());
    }

    fn enqueue(&mut self, fd: c_int) -> bool {
        let c = &mut self.conns[fd as usize];
        if c.live && !c.queued && c.outbuf.is_empty() && !c.close_after {
            if self.ready.try_reserve(1).is_err() { return false; }
            c.queued = true;
            self.ready.push_back((fd, c.generation));
        }
        true
    }

    fn recycle_output(&mut self, mut buf: Vec<u8>) {
        let cap = buf.capacity();
        if cap != 0 && cap <= OUTPUT_POOL_MAX_BUFFER && self.pool.len() < POOL_MAX
            && self.pool_bytes <= OUTPUT_POOL_MAX_BYTES.saturating_sub(cap) {
            if self.pool.try_reserve(1).is_err() { return; }
            buf.clear();
            self.pool_bytes += cap;
            self.pool.push(buf);
        }
    }

    fn release(&mut self, fd: c_int) {
        let ev = sys::event_for(fd, 0);
        unsafe {
            sys::epoll_ctl(self.ep, sys::EPOLL_CTL_DEL, fd, &ev);
            sys::close(fd);
        }
        self.output_bytes -= self.conns[fd as usize].outbuf.capacity();
        let output = mem::take(&mut self.conns[fd as usize].outbuf);
        self.recycle_output(output);
        let input = mem::take(&mut self.conns[fd as usize].inbuf);
        self.conns[fd as usize] = Conn::new();
        self.recycle_input_buffer(input);
    }

    fn recycle_input_buffer(&mut self, mut buf: Vec<u8>) {
        if !self.input_pool_enabled {
            return;
        }
        let capacity = buf.capacity();
        if !input_pool_buffer_allowed(self.input_pool.len(), self.input_pool_bytes, capacity) {
            return;
        }
        if self.input_pool.try_reserve(1).is_err() { return; }
        buf.clear();
        self.input_pool_bytes += capacity;
        self.input_pool.push(buf);
    }

    fn take_input_buffer(&mut self, idx: usize) {
        if self.input_pool_enabled && self.conns[idx].inbuf.capacity() == 0 {
            if let Some(mut buf) = self.input_pool.pop() {
                self.input_pool_bytes -= buf.capacity();
                buf.clear();
                self.conns[idx].inbuf = buf;
            }
        }
    }

    /// Read what is available into the input buffer. false = close.
    fn recv_conn(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        self.take_input_buffer(idx);
        let mut received = 0;
        loop {
            let old = self.conns[idx].inbuf.len();
            if old >= http::MAX_HEAD + http::MAX_BODY || received >= 16 * 1024 {
                break;
            }
            // Grow in small steps: reserving the whole read chunk per
            // connection costs 16 KB of resident memory for every socket, and
            // at 300k sockets that is more than the kernel's own cost. A long
            // request still grows the buffer as it arrives.
            // Most requests are a few hundred bytes: start there and grow only
            // for a request that really is longer. Every socket pays for this.
            let mut cap = self.conns[idx].inbuf.capacity();
            if cap - old < 192 {
                let want = (old + 256).max(256);
                if self.conns[idx].inbuf.try_reserve(want - old).is_err() { return false; }
                cap = self.conns[idx].inbuf.capacity();
            }
            let n = unsafe {
                sys::recv(
                    fd,
                    self.conns[idx].inbuf.as_mut_ptr().add(old) as *mut c_void,
                    (cap - old).min(http::MAX_HEAD + http::MAX_BODY - old).min(16 * 1024 - received),
                    0,
                )
            };
            if n > 0 {
                unsafe { self.conns[idx].inbuf.set_len(old + n as usize) };
                received += n as usize;
                continue;
            }
            self.conns[idx].inbuf.truncate(old);
            if n == 0 {
                self.conns[idx].read_eof = true;
                break;
            }
            if !sys::would_block() {
                return false;
            }
            break;
        }
        true
    }

    /// At most one provisional response per connection. Already committed
    /// output must drain before executing another request on that connection.
    fn serve_conn(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        if !self.conns[idx].outbuf.is_empty() || self.conns[idx].close_after {
            return true;
        }
        let quota = OUTPUT_MAX_BYTES.saturating_sub(self.output_bytes + self.pool_bytes) / 2;
        // Reserve both scratch and final construction before executing writes.
        // If even a small error cannot fit, close without executing this request.
        if quota < ERROR_RESERVE || self.pending.try_reserve(1).is_err() { return false; }
        let reusable = self.pool.pop().map(|b| { self.pool_bytes -= b.capacity(); b }).unwrap_or_default();
        let mut out = buffer::Buffer::from_vec(reusable, quota);
        out.clear();
        // Physically reserve the small failure reply before any request work.
        out.reserve(ERROR_RESERVE);
        if out.failed() { return false; }
        self.app.output_budget(quota);
        let mut input = mem::take(&mut self.conns[idx].inbuf);
        let mut close = false;
        match http::parse(&input) {
            Err(()) => {
                http::respond(&mut out, 400, app::R_INTERNAL);
                input.clear();
                close = true;
            }
            Ok(None) => {
                if !self.conns[idx].continue_sent && http::continue_needed(&input).unwrap_or(false) {
                    out.extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                    self.conns[idx].continue_sent = true;
                }
                if self.conns[idx].read_eof { input.clear(); close = true; }
            }
            Ok(Some((rq, used))) => {
                self.conns[idx].continue_sent = false;
                close = self.app.handle(&rq, &mut out);
                input.drain(..used);
                if close { input.clear(); }
            }
        }
        if out.failed() || self.app.output_failed() {
            // Resource overload: never publish a truncated successful response.
            out.clear();
            http::respond(&mut out, 500, app::R_INTERNAL);
            input.clear();
            close = true;
        }
        self.app.release_scratch();
        if self.conns[idx].read_eof && input.is_empty() { close = true; }
        let output = out.into_vec();
        if output.is_empty() {
            self.recycle_output(output);
        } else {
            self.output_bytes += output.capacity();
            self.conns[idx].outbuf = output;
            self.pending.push(Pending { fd, close_after: close });
        }
        if input.is_empty() {
            // The request parser and handler have finished borrowing `input`.
            // Recycle only a fully consumed buffer; a partial request or a
            // pipeline tail must stay attached to this connection.
            self.recycle_input_buffer(input);
            self.conns[idx].inbuf = Vec::new();
        } else {
            self.conns[idx].inbuf = input;
        }
        // No network wait happens here; end_batch/dispatch run first.
        !close || !self.conns[idx].outbuf.is_empty()
    }

    /// Push pending output. false = close the connection.
    fn flush(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        loop {
            let c = &self.conns[idx];
            if c.sent >= c.outbuf.len() {
                break;
            }
            let n = unsafe {
                sys::send(
                    fd,
                    c.outbuf.as_ptr().add(c.sent) as *const c_void,
                    c.outbuf.len() - c.sent,
                    sys::MSG_NOSIGNAL,
                )
            };
            if n > 0 {
                self.conns[idx].sent += n as usize;
                continue;
            }
            if n < 0 && sys::would_block() {
                if !self.conns[idx].want_out {
                    self.conns[idx].want_out = true;
                    self.arm(fd, sys::EPOLLOUT);
                }
                return true;
            }
            return false;
        }

        let output = mem::take(&mut self.conns[idx].outbuf);
        self.output_bytes -= output.capacity();
        self.recycle_output(output);
        self.conns[idx].sent = 0;
        if self.conns[idx].close_after { return false; }
        self.conns[idx].want_out = false;
        if !self.conns[idx].inbuf.is_empty() {
            if !self.enqueue(fd) { return false; }
        } else if self.conns[idx].read_eof {
            return false;
        }
        self.arm(fd, if self.conns[idx].read_eof { 0 } else { sys::EPOLLIN });
        true
    }

    fn accept_all(&mut self, lfd: c_int) {
        for _ in 0..128 {
            let fd = unsafe {
                sys::accept4(
                    lfd,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    sys::SOCK_NONBLOCK | sys::SOCK_CLOEXEC,
                )
            };
            if fd < 0 {
                // EMFILE/ENFILE/ENOBUFS: this process is full. Hand every
                // further accept to the other workers by leaving the epoll set.
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == 24 || err == 23 || err == 105 || err == 12 {
                    self.accept_retry_at = Some(Instant::now() + Duration::from_millis(250));
                    self.set_accepting(lfd, false);
                    eprintln!("worker full after {} connections: not accepting", self.conns.len());
                }
                return;
            }
            sys::set_nodelay(fd);
            let idx = fd as usize;
            if !self.touch(idx) {
                unsafe { sys::close(fd) };
                self.accept_retry_at = Some(Instant::now() + Duration::from_millis(250));
                self.set_accepting(lfd, false);
                return;
            }
            self.conns[idx] = Conn::new();
            self.conns[idx].live = true;
            self.next_generation = self.next_generation.checked_add(1).expect("connection generation exhausted");
            self.conns[idx].generation = self.next_generation;

            let ev = sys::event_for(fd, sys::EPOLLIN);
            let rc = unsafe { sys::epoll_ctl(self.ep, sys::EPOLL_CTL_ADD, fd, &ev) };
            if rc < 0 {
                unsafe { sys::close(fd) };
                self.conns[idx].live = false;
            }
        }
    }

    /// Responses are already in each connection's output buffer; here the pass
    /// either lets them go out (commit succeeded) or replaces them with a 500
    /// and closes the connection (commit failed, so no write it described can
    /// be reported as done).
    fn dispatch(&mut self, committed: bool) {
        // One pending entry per connection; no sorting or extra metadata
        // allocation is needed after executing the batch.
        let mut pending = mem::take(&mut self.pending);
        for p in pending.drain(..) {
            let idx = p.fd as usize;
            if !self.conns[idx].live { continue; }
            if committed {
                self.conns[idx].close_after = p.close_after;
            } else {
                let old = mem::take(&mut self.conns[idx].outbuf);
                let cap = old.capacity();
                let mut error = buffer::Buffer::from_vec(old, cap);
                error.clear();
                http::respond(&mut error, 500, app::R_INTERNAL);
                // Replacement cannot grow beyond the already charged capacity.
                // If construction nevertheless fails, close without a reply.
                self.output_bytes -= cap;
                if error.failed() {
                    self.release(p.fd);
                    continue;
                }
                self.output_bytes += error.capacity();
                self.conns[idx].outbuf = error.into_vec();
                self.conns[idx].close_after = true;
            }
            if !self.flush(p.fd) { self.release(p.fd); }
        }
        self.pending = pending;
    }

    #[cfg(debug_assertions)]
    fn check_output_accounting(&self) {
        assert_eq!(self.output_bytes, self.conns.iter().map(|c| c.outbuf.capacity()).sum::<usize>());
        assert_eq!(self.pool_bytes, self.pool.iter().map(|b| b.capacity()).sum::<usize>());
        assert!(self.pool_bytes <= OUTPUT_POOL_MAX_BYTES);
        assert!(self.pool.len() <= POOL_MAX);
        assert!(self.pool.iter().all(|b| b.capacity() <= OUTPUT_POOL_MAX_BUFFER));
        assert!(self.output_bytes + self.pool_bytes + self.app.scratch_capacity() <= OUTPUT_MAX_BYTES + 16 * 1024);
    }

    fn run(&mut self, lfd: c_int) -> ! {
        let mut events = [[0u8; 12]; 256];
        let mut ready: Vec<c_int> = Vec::with_capacity(REQUESTS_PER_PASS);
        let mut ready_reads: Vec<c_int> = Vec::with_capacity(REQUESTS_PER_PASS);
        let mut ready_writes: Vec<c_int> = Vec::with_capacity(REQUESTS_PER_PASS);
        loop {
            self.refresh_accepting(lfd);
            let timeout = if !self.ready.is_empty() { 0 } else if self.accepting { -1 } else { 250 };
            let n = unsafe { sys::epoll_wait(self.ep, events.as_mut_ptr(), 256, timeout) };
            if n < 0 {
                if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                panic!("epoll_wait: {}", std::io::Error::last_os_error());
            }
            for i in 0..n as usize {
                let fd = sys::event_fd(&events[i]);
                let mask = sys::event_mask(&events[i]);
                if fd == lfd {
                    self.refresh_accepting(lfd);
                    if self.accepting {
                        self.accept_all(lfd);
                    }
                    continue;
                }
                let idx = fd as usize;
                if idx >= self.conns.len() || !self.conns[idx].live {
                    continue;
                }
                let readable = mask & (sys::EPOLLIN | sys::EPOLLHUP) != 0;
                let dead = mask & sys::EPOLLERR != 0
                    || (readable && self.conns[idx].outbuf.is_empty() && !self.recv_conn(fd))
                    || (mask & (sys::EPOLLOUT | sys::EPOLLHUP) != 0 && !self.flush(fd));
                if dead {
                    self.release(fd);
                } else if readable && !self.enqueue(fd) {
                    self.release(fd);
                }
            }
            while ready.len() < REQUESTS_PER_PASS {
                let Some((fd, generation)) = self.ready.pop_front() else { break; };
                let c = &mut self.conns[fd as usize];
                if !c.live || c.generation != generation { continue; }
                c.queued = false;
                if c.outbuf.is_empty() && !c.close_after { ready.push(fd); }
            }
            // Serve phase: the pass takes the write lock up front if any request
            // in it is a POST, and executes all non-POST reads before writes so
            // B-tree read cursors open at most once and close once before writes.
            ready_reads.clear();
            ready_writes.clear();
            for fd in ready.drain(..) {
                let inb = &self.conns[fd as usize].inbuf;
                if inb.windows(5).any(|w| w == b"POST ") {
                    ready_writes.push(fd);
                } else {
                    ready_reads.push(fd);
                }
            }
            let write = !ready_writes.is_empty();
            self.app.begin_batch(write);
            for fd in ready_reads.drain(..).chain(ready_writes.drain(..)) {
                if !self.serve_conn(fd) {
                    self.release(fd);
                }
            }
            let committed = self.app.end_batch();
            self.dispatch(committed);
            #[cfg(debug_assertions)]
            self.check_output_accounting();
        }
    }
}

fn main() {
    let db_path = match std::env::var("SQLITE_PATH") {
        Ok(v) => v,
        Err(_) => {
            eprintln!("SQLITE_PATH must be set");
            std::process::exit(1);
        }
    };
    let secret = match std::env::var("JWT_SECRET") {
        Ok(v) => v,
        Err(_) => {
            eprintln!("JWT_SECRET must be set");
            std::process::exit(1);
        }
    };
    let host = std::env::var("HOST").unwrap_or_else(|_| String::from("127.0.0.1"));
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(3000);

    sys::raise_nofile();

    let allow_l2 = std::env::var("FEED_EPOLL").is_err()
        && std::env::var("FEED_L2_URING").as_deref() != Ok("0");
    let use_l2 = allow_l2 && l2_uring::self_check();
    if allow_l2 && !use_l2 {
        eprintln!("L2 io_uring self-check failed; falling back to epoll");
    }

    let listeners = match sys::bind_listeners(&host, port, if use_l2 { 4 } else { 1 }) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("cannot bind {host}:{port}: {e}");
            std::process::exit(1);
        }
    };

    if use_l2 {
        let listener_count = listeners.len();
        match l2_server::Server::new(listeners.clone(), &db_path, &secret) {
            Ok(server) => {
                eprintln!(
                    "transport: l2 io_uring (single-owner multi-ring, listeners={listener_count})"
                );
                let _ = std::io::stderr()
                    .write_all(format!("listening on {host}:{port}\n").as_bytes());
                server.run();
            }
            Err(e) => {
                eprintln!("L2 io_uring unavailable ({e}); falling back to epoll");
                for &fd in &listeners[1..] {
                    unsafe { sys::close(fd); }
                }
            }
        }
    }
    let lfd = listeners[0];

    // io_uring lifts the connection ceiling inside one process. It is used only
    // after a live end-to-end self test; otherwise the epoll loop serves the same
    // API. The self test opens and closes its own connection, so it runs before
    // any fork.
    // The archived backend has known UAPI/lifetime/progress defects. Keep its
    // source for the independent repair track; a startup smoke test does not
    // make it a supported release transport.
    const VERIFIED_URING: bool = false;
    let use_uring = VERIFIED_URING
        && std::env::var("FEED_EPOLL").is_err()
        && net_uring::self_test(&secret, &db_path);
    let mut started: Option<App> = None;
    if use_uring {
        eprintln!("transport: io_uring");
        let app = App::new(&db_path, &secret);
        // Returns only when the rings could not be set up after all.
        started = Some(net_uring::serve(app, lfd));
        eprintln!("io_uring: no ring could be opened; serving on epoll in one process");
    }

    // Without io_uring, one epoll process is capped at its descriptor limit.
    // Workers are forked before any SQLite connection or epoll instance exists,
    // so each process has its own descriptor table, its own connection and its
    // own epoll set, and all of them accept on the shared listening socket.
    // A process that already owns a connection must not fork, so a server that
    // fell back after starting its database runs as a single worker.
    let mut workers: usize = std::env::var("FEED_WORKERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(5)
        .clamp(1, 16);
    if started.is_some() {
        workers = 1;
    }
    let mut is_child = false;
    for _ in 1..workers {
        if sys::fork_process() == 0 {
            is_child = true;
            break;
        }
    }
    eprintln!("transport: epoll, worker {} of {}", if is_child { "child" } else { "parent" }, workers);

    let ep = unsafe { sys::epoll_create1(0x80000) }; // EPOLL_CLOEXEC
    assert!(ep >= 0, "epoll_create1 failed");
    let ev = sys::event_for(lfd, sys::EPOLL_IN_FOR_LISTENER);
    unsafe {
        assert!(sys::epoll_ctl(ep, sys::EPOLL_CTL_ADD, lfd, &ev) >= 0);
    }
    let app = match started {
        Some(app) => app,
        None => App::new(&db_path, &secret),
    };
    let mut server = Server {
        ep,
        conns: Vec::new(),
        app,
        pending: Vec::with_capacity(512),
        accepting: true,
        accept_retry_at: None,
        mem: sys::MemGuard::new(),
        pool: Vec::new(),
        input_pool_enabled: std::env::var("FEED_EPOLL_INPUT_POOL")
            .map(|value| value != "0")
            .unwrap_or(true),
        input_pool: Vec::new(),
        input_pool_bytes: 0,
        output_bytes: 0,
        pool_bytes: 0,
        ready: VecDeque::new(),
        next_generation: 0,
    };
    eprintln!("epoll input buffer pool: {}", if server.input_pool_enabled { "enabled" } else { "disabled" });
    let _ = std::io::stderr().write_all(format!("listening on {host}:{port}\n").as_bytes());
    server.run(lfd);
}

#[cfg(test)]
mod input_pool_tests {
    use super::*;

    #[test]
    fn input_pool_enforces_count_capacity_and_byte_bounds() {
        assert!(input_pool_buffer_allowed(0, 0, 1));
        assert!(input_pool_buffer_allowed(INPUT_POOL_MAX_BUFFERS - 1, 0, INPUT_POOL_MAX_BUFFER_BYTES));
        assert!(input_pool_buffer_allowed(0, INPUT_POOL_MAX_BYTES - 1, 1));
        assert!(!input_pool_buffer_allowed(0, 0, 0));
        assert!(!input_pool_buffer_allowed(0, 0, INPUT_POOL_MAX_BUFFER_BYTES + 1));
        assert!(!input_pool_buffer_allowed(INPUT_POOL_MAX_BUFFERS, 0, 1));
        assert!(!input_pool_buffer_allowed(0, INPUT_POOL_MAX_BYTES, 1));
        assert!(!input_pool_buffer_allowed(0, usize::MAX, 1));
    }
}
