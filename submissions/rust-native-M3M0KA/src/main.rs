//! $12 server challenge entry: a native Rust HTTP/1.1 feed server.
//!
//! One thread, one SQLite connection, no framework, no third-party crates.
//! Configuration comes only from SQLITE_PATH, JWT_SECRET, HOST and PORT.
//!
//! One pass of the event loop is one SQLite transaction. Responses are built
//! into a per-pass buffer and copied to their connections only after the commit
//! returns, so a 201 can never describe a write that rolled back.

mod app;
mod btree;
mod crypto;
mod db;
mod http;
mod json;
mod net_uring;
mod sys;
mod uring;

use std::io::Write;
use std::mem;
use std::net::TcpListener;
use std::os::fd::IntoRawFd;
use std::os::raw::{c_int, c_void};

use app::App;

const READ_CHUNK: usize = 16 * 1024;

struct Conn {
    live: bool,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    sent: usize,
    close_after: bool,
    want_out: bool,
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
        }
    }
}

struct Pending {
    fd: c_int,
    start: usize,
    len: usize,
    close_after: bool,
}

struct Server {
    ep: c_int,
    conns: Vec<Conn>,
    app: App,
    pending: Vec<Pending>,
}

impl Server {
    fn touch(&mut self, fd: usize) {
        if fd >= self.conns.len() {
            self.conns.resize_with(fd + 1, Conn::new);
        }
    }

    fn arm(&self, fd: c_int, events: u32) {
        let ev = sys::event_for(fd, events);
        unsafe {
            sys::epoll_ctl(self.ep, sys::EPOLL_CTL_MOD, fd, &ev);
        }
    }

    fn release(&mut self, fd: c_int) {
        let ev = sys::event_for(fd, 0);
        unsafe {
            sys::epoll_ctl(self.ep, sys::EPOLL_CTL_DEL, fd, &ev);
            sys::close(fd);
        }
        self.touch(fd as usize);
        self.conns[fd as usize] = Conn::new();
    }

    /// Read what is available into the input buffer. false = close.
    fn recv_conn(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        self.touch(idx);

        loop {
            let old = self.conns[idx].inbuf.len();
            if old > http::MAX_HEAD + http::MAX_BODY {
                return false;
            }
            self.conns[idx].inbuf.reserve(READ_CHUNK);
            let n = unsafe {
                sys::recv(
                    fd,
                    self.conns[idx].inbuf.as_mut_ptr().add(old) as *mut c_void,
                    READ_CHUNK,
                    0,
                )
            };
            if n > 0 {
                unsafe { self.conns[idx].inbuf.set_len(old + n as usize) };
                continue;
            }
            self.conns[idx].inbuf.truncate(old);
            if n == 0 || !sys::would_block() {
                return false;
            }
            break;
        }
        true
    }

    /// Serve every complete request buffered on the connection. false = close.
    fn serve_conn(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        // Move the input out so a request can borrow it while its response is
        // appended to the (different) per-pass buffer.
        let mut buf = mem::take(&mut self.conns[idx].inbuf);
        if !buf.is_empty() {
            loop {
                match http::parse(&buf) {
                    Ok(None) => break,
                    Err(()) => {
                        let c = &mut self.conns[idx];
                        let start = c.outbuf.len();
                        http::respond(&mut c.outbuf, 400, app::R_INTERNAL);
                        let len = c.outbuf.len() - start;
                        self.pending.push(Pending { fd, start, len, close_after: true });
                        buf.clear();
                        break;
                    }
                    Ok(Some((rq, used))) => {
                        if rq.expect_continue {
                            self.conns[idx]
                                .outbuf
                                .extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                        }
                        let app = &mut self.app;
                        let c = &mut self.conns[idx];
                        let start = c.outbuf.len();
                        let close = app.handle(&rq, &mut c.outbuf);
                        let len = c.outbuf.len() - start;
                        self.pending.push(Pending { fd, start, len, close_after: close });
                        buf.drain(..used);
                        if close {
                            buf.clear();
                            break;
                        }
                    }
                }
            }
        }
        // Put the buffer back even when it is empty: dropping it here frees and
        // reallocates it per request, which glibc turns into mmap/munmap.
        self.conns[idx].inbuf = buf;
        true
    }

    /// Push pending output. false = close the connection.
    fn flush(&mut self, fd: c_int) -> bool {
        let idx = fd as usize;
        self.touch(idx);
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
                    self.arm(fd, sys::EPOLLIN | sys::EPOLLOUT);
                }
                return true;
            }
            return false;
        }

        self.conns[idx].outbuf.clear();
        self.conns[idx].sent = 0;
        if self.conns[idx].close_after {
            return false;
        }
        if self.conns[idx].want_out {
            self.conns[idx].want_out = false;
            self.arm(fd, sys::EPOLLIN);
        }
        true
    }

    fn accept_all(&mut self, lfd: c_int) {
        loop {
            let fd = unsafe {
                sys::accept4(
                    lfd,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    sys::SOCK_NONBLOCK | sys::SOCK_CLOEXEC,
                )
            };
            if fd < 0 {
                return;
            }
            sys::set_nodelay(fd);
            let idx = fd as usize;
            self.touch(idx);
            self.conns[idx] = Conn::new();
            self.conns[idx].live = true;

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
        let mut fds: Vec<c_int> = Vec::with_capacity(self.pending.len());
        if committed {
            for p in self.pending.iter() {
                let idx = p.fd as usize;
                if idx >= self.conns.len() || !self.conns[idx].live {
                    continue;
                }
                if p.close_after {
                    self.conns[idx].close_after = true;
                }
                fds.push(p.fd);
            }
        } else {
            let mut first: Vec<(c_int, usize)> = Vec::new();
            for p in self.pending.iter() {
                match first.iter_mut().find(|(fd, _)| *fd == p.fd) {
                    Some((_, at)) => {
                        if p.start < *at {
                            *at = p.start;
                        }
                    }
                    None => first.push((p.fd, p.start)),
                }
            }
            for (fd, at) in first {
                let idx = fd as usize;
                if idx >= self.conns.len() || !self.conns[idx].live {
                    continue;
                }
                self.conns[idx].outbuf.truncate(at);
                http::respond(&mut self.conns[idx].outbuf, 500, app::R_INTERNAL);
                self.conns[idx].close_after = true;
                fds.push(fd);
            }
        }
        self.pending.clear();
        fds.sort_unstable();
        fds.dedup();
        for fd in fds {
            if (fd as usize) < self.conns.len() && self.conns[fd as usize].live && !self.flush(fd) {
                self.release(fd);
            }
        }
    }

    fn run(&mut self, lfd: c_int) -> ! {
        let mut events = [[0u8; 12]; 256];
        let mut ready: Vec<c_int> = Vec::with_capacity(256);
        loop {
            let n = unsafe { sys::epoll_wait(self.ep, events.as_mut_ptr(), 256, -1) };
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
                    self.accept_all(lfd);
                    continue;
                }
                let idx = fd as usize;
                if idx >= self.conns.len() || !self.conns[idx].live {
                    continue;
                }
                let dead = mask & (sys::EPOLLHUP | sys::EPOLLERR) != 0
                    || (mask & sys::EPOLLIN != 0 && !self.recv_conn(fd))
                    || (mask & sys::EPOLLOUT != 0 && !self.flush(fd));
                if dead {
                    self.release(fd);
                } else if mask & sys::EPOLLIN != 0 {
                    ready.push(fd);
                }
            }
            // Serve phase: the pass takes the write lock up front if any request
            // in it is a POST.
            let write = ready.iter().any(|&fd| {
                let inb = &self.conns[fd as usize].inbuf;
                inb.windows(5).any(|w| w == b"POST ")
            });
            self.app.begin_batch(write);
            for fd in ready.drain(..) {
                if !self.serve_conn(fd) {
                    self.release(fd);
                }
            }
            let committed = self.app.end_batch();
            self.dispatch(committed);
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

    let listener = match TcpListener::bind((host.as_str(), port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("cannot bind {host}:{port}: {e}");
            std::process::exit(1);
        }
    };
    listener.set_nonblocking(true).expect("nonblocking listener");
    let lfd = listener.into_raw_fd();
    sys::set_reuseaddr(lfd);

    // io_uring lifts the connection ceiling inside one process. It is used only
    // after a live end-to-end self test; otherwise the epoll loop serves the same
    // API. The self test opens and closes its own connection, so it runs before
    // any fork.
    let use_uring = std::env::var("FEED_EPOLL").is_err() && net_uring::self_test(&secret, &db_path);
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
    };
    let _ = std::io::stderr().write_all(format!("listening on {host}:{port}\n").as_bytes());
    server.run(lfd);
}
