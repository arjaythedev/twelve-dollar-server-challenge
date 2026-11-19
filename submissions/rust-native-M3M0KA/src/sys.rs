//! Minimal Linux bindings: epoll, non-blocking accept/recv/send, rlimits.
//!
//! Only what the transport needs. Rust's standard library already provides
//! address resolution and `io::Error` (which reads errno correctly), so no
//! libc crate is required anywhere in this project.

use std::os::raw::{c_int, c_void};

pub const EPOLLIN: u32 = 0x0001;
/// With several worker processes waiting on one listening socket, a plain
/// level-triggered read interest wakes all of them for every new connection and
/// all but one find EAGAIN. EPOLLEXCLUSIVE wakes one waiter instead. It is only
/// valid with EPOLL_CTL_ADD, which is how the listener is registered.
pub const EPOLLEXCLUSIVE: u32 = 0x1000_0000;
pub const EPOLL_IN_FOR_LISTENER: u32 = EPOLLIN | EPOLLEXCLUSIVE;
pub const EPOLLOUT: u32 = 0x0004;
pub const EPOLLERR: u32 = 0x0008;
pub const EPOLLHUP: u32 = 0x0010;

pub const EPOLL_CTL_ADD: c_int = 1;
pub const EPOLL_CTL_DEL: c_int = 2;
pub const EPOLL_CTL_MOD: c_int = 3;

pub const SOCK_NONBLOCK: c_int = 0o4000;
pub const SOCK_CLOEXEC: c_int = 0o2000000;

const SOL_SOCKET: c_int = 1;
const SO_REUSEADDR: c_int = 2;
const SO_REUSEPORT: c_int = 15;
const AF_INET: c_int = 2;
const AF_INET6: c_int = 10;
const SOCK_STREAM: c_int = 1;
const IPPROTO_TCP: c_int = 6;
const TCP_NODELAY: c_int = 1;
const RLIMIT_NOFILE: c_int = 7;
const LISTEN_BACKLOG: c_int = 4096;

pub const MSG_NOSIGNAL: c_int = 0x4000;

#[repr(C)]
pub struct Rlimit {
    pub cur: u64,
    pub max: u64,
}

/// `struct epoll_event` is packed to 12 bytes on x86-64 (and on every other
/// 64-bit Linux ABI). Representing it as raw bytes avoids taking references
/// into a packed struct, which Rust considers undefined behaviour.
pub type RawEvent = [u8; 12];

pub fn event_for(fd: c_int, events: u32) -> RawEvent {
    let mut b = [0u8; 12];
    b[0..4].copy_from_slice(&events.to_ne_bytes());
    b[4..12].copy_from_slice(&(fd as u64).to_ne_bytes());
    b
}

pub fn event_mask(e: &RawEvent) -> u32 {
    u32::from_ne_bytes([e[0], e[1], e[2], e[3]])
}

pub fn event_fd(e: &RawEvent) -> c_int {
    let mut raw = [0u8; 8];
    raw.copy_from_slice(&e[4..12]);
    u64::from_ne_bytes(raw) as c_int
}

extern "C" {
    pub fn epoll_create1(flags: c_int) -> c_int;
    pub fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *const RawEvent) -> c_int;
    pub fn epoll_wait(epfd: c_int, events: *mut RawEvent, maxevents: c_int, timeout: c_int)
        -> c_int;
    pub fn accept4(fd: c_int, addr: *mut c_void, addrlen: *mut u32, flags: c_int) -> c_int;
    pub fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
    fn fork() -> c_int;
    pub fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    pub fn close(fd: c_int) -> c_int;
    pub fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
    pub fn bind(fd: c_int, addr: *const c_void, len: u32) -> c_int;
    pub fn listen(fd: c_int, backlog: c_int) -> c_int;
    pub fn setsockopt(fd: c_int, level: c_int, name: c_int, val: *const c_void, len: u32) -> c_int;
    pub fn getrlimit(resource: c_int, rlim: *mut Rlimit) -> c_int;
    pub fn setrlimit(resource: c_int, rlim: *const Rlimit) -> c_int;
}

/// Raise the soft descriptor limit to the inherited hard limit. The challenge
/// service starts us with a hard limit of 65,535; a larger one is used as is.
pub fn raise_nofile() {
    unsafe {
        let mut rl = Rlimit { cur: 0, max: 0 };
        if getrlimit(RLIMIT_NOFILE, &mut rl) == 0 && rl.cur < rl.max {
            let want = Rlimit { cur: rl.max, max: rl.max };
            setrlimit(RLIMIT_NOFILE, &want);
        }
    }
}

pub fn set_nodelay(fd: c_int) {
    let one: c_int = 1;
    unsafe {
        setsockopt(
            fd,
            IPPROTO_TCP,
            TCP_NODELAY,
            &one as *const c_int as *const c_void,
            std::mem::size_of::<c_int>() as u32,
        );
    }
}

pub fn set_reuseaddr(fd: c_int) {
    let one: c_int = 1;
    unsafe {
        setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &one as *const c_int as *const c_void,
            std::mem::size_of::<c_int>() as u32,
        );
    }
}

#[repr(C)]
struct SockAddrIn {
    sin_family: u16,
    sin_port: u16,
    sin_addr: u32,
    sin_zero: [u8; 8],
}

#[repr(C)]
struct SockAddrIn6 {
    sin6_family: u16,
    sin6_port: u16,
    sin6_flowinfo: u32,
    sin6_addr: [u8; 16],
    sin6_scope_id: u32,
}

fn bind_addr(addr: std::net::SocketAddr, count: usize) -> std::io::Result<Vec<c_int>> {
    let target = if addr.port() == 0 { 1 } else { count.max(1) };
    let (family, sa_v4, sa_v6, addr_len): (c_int, SockAddrIn, SockAddrIn6, u32) = match addr {
        std::net::SocketAddr::V4(v4) => (
            AF_INET,
            SockAddrIn {
                sin_family: AF_INET as u16,
                sin_port: v4.port().to_be(),
                sin_addr: u32::from_ne_bytes(v4.ip().octets()),
                sin_zero: [0; 8],
            },
            SockAddrIn6 { sin6_family: 0, sin6_port: 0, sin6_flowinfo: 0, sin6_addr: [0; 16], sin6_scope_id: 0 },
            std::mem::size_of::<SockAddrIn>() as u32,
        ),
        std::net::SocketAddr::V6(v6) => (
            AF_INET6,
            SockAddrIn { sin_family: 0, sin_port: 0, sin_addr: 0, sin_zero: [0; 8] },
            SockAddrIn6 {
                sin6_family: AF_INET6 as u16,
                sin6_port: v6.port().to_be(),
                sin6_flowinfo: v6.flowinfo(),
                sin6_addr: v6.ip().octets(),
                sin6_scope_id: v6.scope_id(),
            },
            std::mem::size_of::<SockAddrIn6>() as u32,
        ),
    };
    let addr_ptr: *const c_void = if family == AF_INET {
        &sa_v4 as *const SockAddrIn as *const c_void
    } else {
        &sa_v6 as *const SockAddrIn6 as *const c_void
    };

    let mut fds = Vec::with_capacity(target);
    let one: c_int = 1;
    let mut reuseport_failed = false;

    for i in 0..target {
        let fd = unsafe { socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };
        if fd < 0 {
            if i == 0 {
                return Err(std::io::Error::last_os_error());
            }
            for &extra in &fds[1..] {
                unsafe { close(extra); }
            }
            fds.truncate(1);
            return Ok(fds);
        }
        set_reuseaddr(fd);
        set_nodelay(fd);
        if target > 1 {
            let rc = unsafe {
                setsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_REUSEPORT,
                    &one as *const c_int as *const c_void,
                    std::mem::size_of::<c_int>() as u32,
                )
            };
            if rc < 0 {
                if i == 0 {
                    reuseport_failed = true;
                } else {
                    unsafe { close(fd); }
                    for &extra in &fds[1..] {
                        unsafe { close(extra); }
                    }
                    fds.truncate(1);
                    return Ok(fds);
                }
            }
        }
        if unsafe { bind(fd, addr_ptr, addr_len) } < 0 || unsafe { listen(fd, LISTEN_BACKLOG) } < 0 {
            let err = std::io::Error::last_os_error();
            unsafe { close(fd); }
            if i == 0 {
                return Err(err);
            }
            for &extra in &fds[1..] {
                unsafe { close(extra); }
            }
            fds.truncate(1);
            return Ok(fds);
        }
        fds.push(fd);
        if reuseport_failed {
            break;
        }
    }
    Ok(fds)
}

/// Binds up to `count` nonblocking `SO_REUSEPORT` TCP listeners with a 4,096
/// SYN/accept backlog and inherited `TCP_NODELAY`, falling back to a single
/// 4,096-backlog listener if `SO_REUSEPORT` or secondary sockets fail.
pub fn bind_listeners(host: &str, port: u16, count: usize) -> std::io::Result<Vec<c_int>> {
    use std::net::ToSocketAddrs;
    let addrs: Vec<std::net::SocketAddr> = (host, port).to_socket_addrs()?.collect();
    if addrs.is_empty() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "no socket address resolved",
        ));
    }
    let mut last_err = None;
    for addr in addrs {
        match bind_addr(addr, count) {
            Ok(fds) => return Ok(fds),
            Err(e) => last_err = Some(e),
        }
    }
    Err(last_err.unwrap_or_else(|| std::io::Error::new(std::io::ErrorKind::Other, "bind failed")))
}

/// The soft descriptor limit of this process (what the service unit sets).
pub fn nofile_limit() -> u32 {
    #[repr(C)]
    struct RLimit {
        cur: u64,
        max: u64,
    }
    extern "C" {
        fn getrlimit(resource: c_int, rlim: *mut RLimit) -> c_int;
    }
    const RLIMIT_NOFILE: c_int = 7;
    let mut r = RLimit { cur: 0, max: 0 };
    let rc = unsafe { getrlimit(RLIMIT_NOFILE, &mut r) };
    if rc != 0 || r.cur == 0 {
        return 1024;
    }
    r.cur.min(u32::MAX as u64) as u32
}

/// fork(2). Returns the child pid in the parent, 0 in the child.
pub fn fork_process() -> c_int {
    let pid = unsafe { fork() };
    if pid < 0 {
        panic!("fork failed: {}", std::io::Error::last_os_error());
    }
    pid
}

/// Stops and resumes accepting on system-wide free memory ("MemAvailable").
///
/// An out-of-memory kill scores zero; a short hold only delays new connects.
/// `MemAvailable` is read at most every ~100 ms, so a hot loop pays one branch
/// per pass and one `open`+parse per hundred. Failure to read counts as "no
/// constraint": guard only what can be measured.
pub struct MemGuard {
    stop_kib: u64,
    resume_kib: u64,
    holding: bool,
    last: std::time::Instant,
}

impl MemGuard {
    /// Threshold defaults: holds below ~160 MB of free memory, resumes above
    /// ~240 MB (hysteresis so a borderline value does not flap the listener).
    pub fn new() -> Self {
        let stop = std::env::var("FEED_MEM_STOP_KIB")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(163_840);
        let resume = std::env::var("FEED_MEM_RESUME_KIB")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(245_760);
        MemGuard {
            stop_kib: stop,
            resume_kib: resume,
            holding: false,
            last: std::time::Instant::now(),
        }
    }

    fn available_kib() -> Option<u64> {
        let s = std::fs::read_to_string("/proc/meminfo").ok()?;
        for line in s.lines() {
            let head = "MemAvailable:";
            if line.starts_with(head) {
                let rest = line[head.len()..].trim_start();
                return rest.split(' ').next()?.parse().ok();
            }
        }
        None
    }

    /// Whether accepts may be armed right now.
    pub fn allows_accepts(&mut self) -> bool {
        let now = std::time::Instant::now();
        if now.duration_since(self.last) >= std::time::Duration::from_millis(100) {
            self.last = now;
            let avail = Self::available_kib();
            self.apply(avail);
        }
        !self.holding
    }

    /// Updates the hold state from a `MemAvailable` reading in kB. An
    /// unreadable value keeps the previous state.
    fn apply(&mut self, avail: Option<u64>) {
        if let Some(avail) = avail {
            if avail <= self.stop_kib {
                self.holding = true;
            } else if avail >= self.resume_kib {
                self.holding = false;
            }
        }
    }
}

#[cfg(test)]
mod mem_tests {
    use super::*;

    fn guard() -> MemGuard {
        MemGuard {
            stop_kib: 100,
            resume_kib: 200,
            holding: false,
            last: std::time::Instant::now(),
        }
    }

    #[test]
    fn holds_low_recovers_high_keeps_between() {
        let mut g = guard();
        g.apply(Some(50));
        assert!(g.holding);
        g.apply(Some(150));
        assert!(g.holding, "between thresholds: keep holding");
        g.apply(Some(300));
        assert!(!g.holding);
    }

    #[test]
    fn unreadable_keeps_state() {
        let mut g = guard();
        g.apply(None);
        assert!(!g.holding);
        g.apply(Some(50));
        g.apply(None);
        assert!(g.holding, "not even an error can lift a hold");
    }
}

/// Drains a counter-style descriptor (eventfd). Reads fail harmlessly with
/// EAGAIN when the counter is already zero.
pub fn drain_counter(fd: c_int) {
    let mut v: u64 = 0;
    unsafe {
        read(fd, &mut v as *mut u64 as *mut c_void, 8);
    }
}

pub fn would_block() -> bool {
    std::io::Error::last_os_error().kind() == std::io::ErrorKind::WouldBlock
}

#[cfg(test)]
mod listener_tests {
    use super::*;

    #[test]
    fn binds_reuseport_listeners_and_accepts() {
        let probe = std::net::TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = probe.local_addr().unwrap().port();
        drop(probe);
        let fds = bind_listeners("127.0.0.1", port, 4).unwrap();
        assert!(fds.len() == 4 || fds.len() == 1, "unexpected listener count {}", fds.len());
        let client = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
        drop(client);
        for fd in fds {
            assert!(fd >= 0);
            unsafe { close(fd); }
        }
    }
}
