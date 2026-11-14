//! Linux only: the network layer under server.ts. Rust owns the listening socket and every connection on one
//! epoll set; the JS thread itself blocks in px_wait (a plain synchronous FFI call), so a request never goes
//! through Deno's event loop, ops or promises. /health floor at 4K paced req/s: 58 -> 34 us/req, RSS
//! 200 -> 65 MB at 10K connections (docs: phase5-findings, C poller; this is its port).
//!
//! px_wait reads every ready connection into its buffer and copies each buffer that got new bytes into the
//! JS-owned `in` array, with (fd, offset, length) per entry in `idx`. JS parses as many complete requests as
//! it finds, answers them with px_send, and writes the bytes it consumed (-1: close) back over the length
//! slot; the next px_wait drops those bytes from the connection's buffer. HTTP parsing stays in JS.
//!
//! Wake (px_wake_open): an eventfd in the epoll set that render.c's commit thread writes when a COMMIT lands,
//! so px_wait returns (0 entries if nothing else is ready) and JS releases the responses waiting on it.
//!
//! No crates: build.sh runs `rustc -O --crate-type cdylib poller.rs`, and the few syscalls are declared below.
use std::ffi::{c_char, c_int, c_void, CStr};
use std::io::{Error, ErrorKind};
use std::net::Ipv4Addr;
use std::ptr::{addr_of_mut, null_mut};
use std::slice;

const MAXFD: usize = 65536;
const RMIN: usize = 1024; // idle read buffer
const RMAX: usize = 32 * 1024; // headers + a 16 KB body, server.ts's MAX_BUF
const WMAX: usize = 1 << 20; // queued response bytes before we give up on a client that does not read
const MAXREQ: usize = 4096; // entries per batch: idx holds 3 ints each
// Fds kept free below RLIMIT_NOFILE: SQLite's, Deno's, and room to accept users who will get one answer and a
// close. Past that many connections, px_crowded tells JS to close connections after their response.
const HEADROOM: usize = 1024;
const RLIMIT_NOFILE: c_int = 7;
const NEVENTS: usize = 512;
const CLOCK_MONOTONIC: c_int = 1;
const WAKE_TAG: u64 = u64::MAX; // epoll data of the wake eventfd (connections and the listener carry their fd)
const EFD_NONBLOCK: c_int = 0o4000;
const EFD_CLOEXEC: c_int = 0o2000000;

const AF_INET: c_int = 2;
const SOCK_STREAM: c_int = 1;
const SOCK_NONBLOCK: c_int = 0o4000;
const SOL_SOCKET: c_int = 1;
const SO_REUSEADDR: c_int = 2;
const IPPROTO_TCP: c_int = 6;
const TCP_NODELAY: c_int = 1;
const EPOLL_CTL_ADD: c_int = 1;
const EPOLL_CTL_DEL: c_int = 2;
const EPOLL_CTL_MOD: c_int = 3;
const EPOLLIN: u32 = 0x1;
const EPOLLOUT: u32 = 0x4;
const EPOLLERR: u32 = 0x8;
const EPOLLHUP: u32 = 0x10;

// x86_64's struct epoll_event is packed (12 bytes); other arches pad it to 16.
#[cfg_attr(target_arch = "x86_64", repr(C, packed))]
#[cfg_attr(not(target_arch = "x86_64"), repr(C))]
#[derive(Clone, Copy)]
struct EpollEvent {
    events: u32,
    data: u64,
}

#[repr(C)]
struct SockaddrIn {
    family: u16,
    port: u16, // network order
    addr: [u8; 4],
    zero: [u8; 8],
}

extern "C" {
    fn socket(domain: c_int, ty: c_int, proto: c_int) -> c_int;
    fn setsockopt(fd: c_int, level: c_int, name: c_int, val: *const c_void, len: u32) -> c_int;
    fn bind(fd: c_int, addr: *const SockaddrIn, len: u32) -> c_int;
    fn listen(fd: c_int, backlog: c_int) -> c_int;
    fn accept4(fd: c_int, addr: *mut c_void, len: *mut u32, flags: c_int) -> c_int;
    fn epoll_create1(flags: c_int) -> c_int;
    fn epoll_ctl(ep: c_int, op: c_int, fd: c_int, ev: *mut EpollEvent) -> c_int;
    fn epoll_wait(ep: c_int, ev: *mut EpollEvent, max: c_int, timeout: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut u8, n: usize) -> isize;
    fn write(fd: c_int, buf: *const u8, n: usize) -> isize;
    fn close(fd: c_int) -> c_int;
    fn clock_gettime(clock: c_int, t: *mut [i64; 2]) -> c_int;
    fn eventfd(init: u32, flags: c_int) -> c_int;
    fn getrlimit(res: c_int, lim: *mut [u64; 2]) -> c_int;
}

fn now_ms() -> i64 {
    let mut t = [0i64; 2];
    unsafe { clock_gettime(CLOCK_MONOTONIC, &mut t) };
    t[0] * 1000 + t[1] / 1_000_000
}

/// r holds unconsumed request bytes (capacity 1 KB, doubling up to RMAX); w the response bytes the socket
/// has not taken yet (empty and unallocated almost always). closing: drop it once w is written.
struct Conn {
    r: Vec<u8>,
    w: Vec<u8>,
    closing: bool,
}

struct State {
    conns: [Option<Box<Conn>>; MAXFD], // None: not open
    ev: [EpollEvent; NEVENTS],
    ep: c_int,
    lfd: c_int,
    inb: *mut u8,
    incap: usize,
    idx: *mut i32,
    last: usize,
    paused: i64, // ms: accepting resumes then (0: accepting)
    lastlog: i64,
    efd: c_int, // wake eventfd (0: px_wake_open not called)
    gens: [u32; MAXFD], // connections accepted on each fd: JS holds responses past a batch, and fds get reused
    nconn: usize, // open connections
    crowd: usize, // more open than this: crowded (set by px_start from RLIMIT_NOFILE)
}

const CLOSED: Option<Box<Conn>> = None;
// One JS thread calls in, one call at a time. All zeros, so it lands in .bss and untouched pages of conns
// cost no RSS (an Option<Conn> table would be 3 MB of .data: None is not all zeros).
static mut S: State = State {
    conns: [CLOSED; MAXFD],
    ev: [EpollEvent { events: 0, data: 0 }; NEVENTS],
    ep: 0,
    lfd: 0,
    inb: null_mut(),
    incap: 0,
    idx: null_mut(),
    last: 0,
    paused: 0,
    lastlog: 0,
    efd: 0,
    gens: [0; MAXFD],
    nconn: 0,
    crowd: 0,
};

fn st() -> &'static mut State {
    unsafe { &mut *addr_of_mut!(S) }
}

fn watch(ep: c_int, fd: usize, op: c_int, events: u32) {
    let mut e = EpollEvent { events, data: fd as u64 };
    unsafe { epoll_ctl(ep, op, fd as c_int, &mut e) };
}

/// Writes until done or EAGAIN; the bytes written. None: the connection is broken.
fn write_some(fd: usize, buf: &[u8]) -> Option<usize> {
    let mut off = 0;
    while off < buf.len() {
        let r = unsafe { write(fd as c_int, buf[off..].as_ptr(), buf.len() - off) };
        if r > 0 {
            off += r as usize;
            continue;
        }
        if r < 0 {
            match Error::last_os_error().kind() {
                ErrorKind::Interrupted => continue,
                ErrorKind::WouldBlock => break,
                _ => {}
            }
        }
        return None;
    }
    Some(off)
}

impl State {
    fn drop(&mut self, fd: usize) {
        if self.conns[fd].take().is_some() {
            unsafe { close(fd as c_int) };
            self.nconn -= 1;
        }
    }

    fn accept_all(&mut self) {
        let one: c_int = 1;
        loop {
            let fd = unsafe { accept4(self.lfd, null_mut(), null_mut(), SOCK_NONBLOCK) };
            if fd < 0 {
                let e = Error::last_os_error();
                match e.kind() {
                    ErrorKind::Interrupted | ErrorKind::ConnectionAborted => continue,
                    ErrorKind::WouldBlock => return,
                    _ => {}
                }
                // EMFILE and friends: stop watching the listener for 10 ms (the backlog holds the rest), log <= 1/s
                let t = now_ms();
                if t - self.lastlog >= 1000 {
                    self.lastlog = t;
                    eprintln!("accept: {e}");
                }
                unsafe { epoll_ctl(self.ep, EPOLL_CTL_DEL, self.lfd, null_mut()) };
                self.paused = t + 10;
                return;
            }
            let mut r = Vec::new();
            if fd as usize >= MAXFD || r.try_reserve_exact(RMIN).is_err() {
                unsafe { close(fd) };
                continue;
            }
            unsafe { setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one as *const c_int as *const c_void, 4) };
            self.conns[fd as usize] = Some(Box::new(Conn { r, w: Vec::new(), closing: false }));
            self.nconn += 1;
            self.gens[fd as usize] = self.gens[fd as usize].wrapping_add(1);
            watch(self.ep, fd as usize, EPOLL_CTL_ADD, EPOLLIN);
        }
    }

    /// Writes the queue; once empty, stops watching for EPOLLOUT.
    fn flush(&mut self, fd: usize) {
        let Some(k) = self.conns[fd].as_mut() else { return };
        let Some(off) = write_some(fd, &k.w) else { return self.drop(fd) };
        if off == k.w.len() {
            if k.closing {
                return self.drop(fd);
            }
            k.w = Vec::new();
            watch(self.ep, fd, EPOLL_CTL_MOD, EPOLLIN);
        } else {
            k.w.drain(..off);
        }
    }
}

/// host: dotted IPv4. 0 on success.
#[no_mangle]
pub extern "C" fn px_start(host: *const c_char, port: c_int, inbuf: *mut u8, inlen: c_int, index: *mut i32) -> c_int {
    let s = st();
    let one: c_int = 1;
    (s.inb, s.incap, s.idx) = (inbuf, inlen as usize, index);
    let mut lim = [MAXFD as u64; 2];
    unsafe { getrlimit(RLIMIT_NOFILE, &mut lim) };
    s.crowd = (lim[0].min(MAXFD as u64) as usize).saturating_sub(HEADROOM);
    let Some(ip) = unsafe { CStr::from_ptr(host) }.to_str().ok().and_then(|h| h.parse::<Ipv4Addr>().ok()) else {
        return -2;
    };
    let a = SockaddrIn { family: AF_INET as u16, port: (port as u16).to_be(), addr: ip.octets(), zero: [0; 8] };
    unsafe {
        s.lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        setsockopt(s.lfd, SOL_SOCKET, SO_REUSEADDR, &one as *const c_int as *const c_void, 4);
        if bind(s.lfd, &a, std::mem::size_of::<SockaddrIn>() as u32) != 0 || listen(s.lfd, 4096) != 0 {
            return -1;
        }
        s.ep = epoll_create1(0);
    }
    watch(s.ep, s.lfd as usize, EPOLL_CTL_ADD, EPOLLIN);
    0
}

/// One response: written now if the socket takes it, else the rest is queued in order. close: the response
/// says `connection: close`, so drop the connection once it is written. -1: connection gone.
#[no_mangle]
pub extern "C" fn px_send(fd: c_int, p: *const u8, n: c_int, close: c_int) -> c_int {
    let s = st();
    let fd = fd as usize;
    let p = unsafe { slice::from_raw_parts(p, n as usize) };
    let Some(k) = s.conns[fd].as_mut() else { return -1 };
    let mut w = 0;
    if k.w.is_empty() {
        let Some(n) = write_some(fd, p) else {
            s.drop(fd);
            return -1;
        };
        w = n;
    }
    let rest = &p[w..];
    if rest.is_empty() {
        // ponytail: close() with a pipelined request unread sends RST and can cut this response short; the
        // load generators never pipeline. shutdown(SHUT_WR), then drop on EOF, if a client ever does.
        if close != 0 {
            s.drop(fd);
        }
        return 0;
    }
    k.closing |= close != 0;
    if k.w.len() + rest.len() > WMAX || k.w.try_reserve(rest.len()).is_err() {
        s.drop(fd);
        return -1;
    }
    if k.w.is_empty() {
        watch(s.ep, fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
    }
    k.w.extend_from_slice(rest);
    0
}

/// Waits up to timeout ms (-1: forever). Returns the number of entries in idx, 0 on timeout or when only the
/// wake fired, -1 on error.
#[no_mangle]
pub extern "C" fn px_wait(timeout: c_int) -> c_int {
    let s = st();
    let idx = unsafe { slice::from_raw_parts_mut(s.idx, 3 * MAXREQ) };
    let inb = unsafe { slice::from_raw_parts_mut(s.inb, s.incap) };
    for e in idx[..3 * s.last].chunks(3) { // what JS consumed from the previous batch
        let (fd, used) = (e[0] as usize, e[2]);
        if used < 0 {
            s.drop(fd);
            continue;
        }
        let Some(k) = s.conns[fd].as_mut() else { continue }; // px_send dropped it
        k.r.drain(..used as usize);
        if k.r.capacity() > RMIN && k.r.len() <= RMIN {
            k.r.shrink_to(RMIN);
        }
    }
    s.last = 0;
    loop {
        let mut t = timeout;
        if s.paused != 0 {
            let now = now_ms();
            if now >= s.paused {
                s.paused = 0;
                watch(s.ep, s.lfd as usize, EPOLL_CTL_ADD, EPOLLIN);
                s.accept_all();
            } else if t < 0 || t as i64 > s.paused - now {
                t = (s.paused - now) as c_int;
            }
        }
        let n = unsafe { epoll_wait(s.ep, s.ev.as_mut_ptr(), NEVENTS as c_int, t) };
        if n < 0 {
            if Error::last_os_error().kind() == ErrorKind::Interrupted {
                continue;
            }
            return -1;
        }
        if n == 0 {
            return 0;
        }
        let (mut used, mut woken) = (0, false);
        for i in 0..n as usize {
            let EpollEvent { events, data } = s.ev[i];
            if data == WAKE_TAG {
                let mut v = 0u64;
                unsafe { read(s.efd, &mut v as *mut u64 as *mut u8, 8) };
                woken = true;
                continue;
            }
            let fd = data as usize;
            if fd == s.lfd as usize {
                s.accept_all();
                continue;
            }
            if events & EPOLLOUT != 0 {
                s.flush(fd);
            }
            if events & (EPOLLIN | EPOLLHUP | EPOLLERR) == 0 {
                continue;
            }
            let Some(k) = s.conns[fd].as_mut() else { continue };
            let cap = k.r.capacity();
            if k.r.len() == cap && (cap >= RMAX || k.r.try_reserve_exact(cap).is_err()) {
                s.drop(fd); // a request that does not fit in RMAX
                continue;
            }
            // Batch full: leave the rest; level-triggered epoll reports them again next call.
            if s.last == MAXREQ || used + k.r.capacity() > s.incap {
                break;
            }
            let spare = k.r.spare_capacity_mut();
            let r = unsafe { read(fd as c_int, spare.as_mut_ptr() as *mut u8, spare.len()) };
            if r < 0 && matches!(Error::last_os_error().kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) {
                continue;
            }
            if r <= 0 {
                s.drop(fd);
                continue;
            }
            unsafe { k.r.set_len(k.r.len() + r as usize) };
            let len = k.r.len();
            inb[used..used + len].copy_from_slice(&k.r);
            idx[3 * s.last..3 * s.last + 3].copy_from_slice(&[fd as i32, used as i32, len as i32]);
            s.last += 1;
            used += len;
        }
        if s.last > 0 || woken {
            return s.last as c_int; // 0 after a wake with no request data: JS still releases what it holds
        }
    }
}

/// Creates the wake eventfd and adds it to the epoll set; returns it for the thread that writes it, -1 on failure.
#[no_mangle]
pub extern "C" fn px_wake_open() -> c_int {
    let s = st();
    let efd = unsafe { eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) };
    if efd < 0 {
        return -1;
    }
    s.efd = efd;
    let mut e = EpollEvent { events: EPOLLIN, data: WAKE_TAG };
    unsafe { epoll_ctl(s.ep, EPOLL_CTL_ADD, efd, &mut e) };
    efd
}

/// fd's generation: it changes whenever a new connection gets fd, so a response held for an older one (or
/// for none: one read after its drop) never reaches it.
#[no_mangle]
pub extern "C" fn px_gen(fd: c_int) -> u32 {
    st().gens[fd as usize]
}

/// 1 when more connections are open than the fd limit leaves room for: JS answers with `connection: close`
/// so users above the limit reconnect per request instead of waiting in the listen queue.
#[no_mangle]
pub extern "C" fn px_crowded() -> c_int {
    let s = st();
    (s.nconn > s.crowd) as c_int
}

/// Closes a connection now (a COMMIT its response waited on failed).
#[no_mangle]
pub extern "C" fn px_close(fd: c_int) {
    st().drop(fd as usize);
}
