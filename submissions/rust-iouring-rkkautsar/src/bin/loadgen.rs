use std::cmp::Reverse;
use std::collections::BinaryHeap;
use std::env;
use std::fs;
use std::io;
use std::mem;
use std::net::Ipv4Addr;
use std::os::fd::RawFd;
use std::sync::OnceLock;
use std::time::Instant;

use libc::{c_int, c_void};

const NB: usize = 100_000;
const READ_BUF: usize = 65_536;
const EPOLL_EVENTS: usize = 1024;
const IP_BIND_ADDRESS_NO_PORT: c_int = 24;

static START: OnceLock<Instant> = OnceLock::new();

fn now_us() -> u64 {
    START.get().unwrap().elapsed().as_micros() as u64
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
    fn range(&mut self, lo: u64, hi: u64) -> u64 {
        lo + self.next() % (hi - lo)
    }
}

#[derive(Clone)]
struct User {
    token: String,
    name: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Sleep,
    Retry,
    Connecting,
    Waiting,
    Closed,
}

struct Vu {
    fd: RawFd,
    state: State,
    up: bool,
    phase: u8,
    user: usize,
    post_id: i64,
    lat_start: u64,
    inflight: bool,
    started: bool,
    timer_gen: u64,
    iteration: u64,
    out: Vec<u8>,
    out_off: usize,
    pending: Vec<u8>,
}

impl Vu {
    fn new(user: usize) -> Self {
        Self {
            fd: -1,
            state: State::Retry,
            up: false,
            phase: 0,
            user,
            post_id: 0,
            lat_start: 0,
            inflight: false,
            started: false,
            timer_gen: 0,
            iteration: 0,
            out: Vec::new(),
            out_off: 0,
            pending: Vec::new(),
        }
    }
}

struct Ctx {
    vus: Vec<Vu>,
    users: Vec<User>,
    heap: BinaryHeap<Reverse<(u64, usize, u64)>>,
    request_deadlines: BinaryHeap<Reverse<(u64, usize, u64)>>,
    ep: RawFd,
    srv: libc::sockaddr_in,
    host_header: String,
    nsrc: usize,
    src_base: usize,
    vu_offset: usize,
    n: usize,
    rng: Rng,
    active: i64,
    connects: u64,
    bind_fail: u64,
    connect_fail: u64,
    epoll_fail: u64,
    conn_timeouts: u64,
    peak_active: i64,
    min_active: i64,
    plateau_start: Option<u64>,
    plateau_end: u64,
    connect_deadline: u64,
    requests: u64,
    errors: u64,
    inflight_count: usize,
    unfinished: u64,
    request_timeouts: u64,
    total_requests: u64,
    hist: Vec<u64>,
    phase_hist: [[u64; 101]; 4],
    timer_lag_ms: Vec<u64>,
    read_batch_lag_ms: Vec<u64>,
    packet_queue_lag_ms: Vec<u64>,
    max_us: u64,
}

impl Ctx {
    fn schedule(&mut self, idx: usize, delay_us: u64) {
        self.vus[idx].timer_gen = self.vus[idx].timer_gen.wrapping_add(1);
        self.heap.push(Reverse((now_us() + delay_us, idx, self.vus[idx].timer_gen)));
    }

    fn close_vu(&mut self, idx: usize, retry: bool) {
        if retry && self.vus[idx].inflight {
            let started = self.vus[idx].lat_start;
            if self.plateau_start.map(|p| started >= p && started < self.plateau_end).unwrap_or(false) {
                self.requests += 1;
                self.errors += 1;
            }
            self.total_requests += 1;
            self.inflight_count -= 1;
            self.vus[idx].inflight = false;
        }
        let failed_feed = retry && self.vus[idx].state == State::Waiting
            && self.vus[idx].phase == 0;
        let v = &mut self.vus[idx];
        if v.fd >= 0 {
            unsafe {
                libc::epoll_ctl(self.ep, libc::EPOLL_CTL_DEL, v.fd, std::ptr::null_mut());
                libc::close(v.fd);
            }
            v.fd = -1;
        }
        if v.up {
            v.up = false;
            self.active -= 1;
            if self.active < self.min_active
                && self.plateau_start.is_some()
                && now_us() < self.plateau_end
            {
                self.min_active = self.active;
            }
        }
        v.timer_gen = v.timer_gen.wrapping_add(1);
        v.state = if retry { State::Retry } else { State::Closed };
        v.out.clear();
        v.out_off = 0;
        v.pending.clear();
        if retry {
            let delay = if failed_feed {
                self.rng.range(3_000_000, 7_000_000)
            } else {
                self.rng.range(20_000, 80_000)
            };
            self.schedule(idx, delay);
        }
    }

    fn start_connect(&mut self, idx: usize) {
        self.close_vu(idx, false);
        let fd = unsafe {
            libc::socket(
                libc::AF_INET,
                libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                0,
            )
        };
        if fd < 0 {
            self.close_vu(idx, true);
            return;
        }
        let one: c_int = 1;
        unsafe {
            libc::setsockopt(
                fd,
                libc::IPPROTO_TCP,
                libc::TCP_NODELAY,
                &one as *const _ as *const c_void,
                mem::size_of::<c_int>() as libc::socklen_t,
            );
            if idx % 256 == 0 {
                libc::setsockopt(
                    fd, libc::SOL_SOCKET, libc::SO_TIMESTAMPNS,
                    &one as *const _ as *const c_void,
                    mem::size_of::<c_int>() as libc::socklen_t,
                );
            }
        }
        if self.nsrc != 0 {
            let src = Ipv4Addr::new(
                127, 0, 0, 2 + self.src_base as u8 + (idx % self.nsrc) as u8,
            );
            unsafe {
                libc::setsockopt(
                    fd,
                    libc::IPPROTO_IP,
                    IP_BIND_ADDRESS_NO_PORT,
                    &one as *const _ as *const c_void,
                    mem::size_of::<c_int>() as libc::socklen_t,
                );
            }
            let mut la: libc::sockaddr_in = unsafe { mem::zeroed() };
            la.sin_family = libc::AF_INET as u16;
            la.sin_addr.s_addr = u32::from_ne_bytes(src.octets());
            if unsafe { libc::bind(fd, &la as *const _ as *const libc::sockaddr, mem::size_of::<libc::sockaddr_in>() as u32) } < 0 {
                self.bind_fail += 1;
                unsafe { libc::close(fd) };
                self.close_vu(idx, true);
                return;
            }
        }
        let rc = unsafe {
            libc::connect(
                fd,
                &self.srv as *const _ as *const libc::sockaddr,
                mem::size_of::<libc::sockaddr_in>() as u32,
            )
        };
        if rc < 0 {
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(libc::EINPROGRESS) {
                self.connect_fail += 1;
                unsafe { libc::close(fd) };
                self.close_vu(idx, true);
                return;
            }
        }
        self.vus[idx].fd = fd;
        self.vus[idx].state = State::Connecting;
        self.vus[idx].up = false;
        let mut ev = libc::epoll_event {
            events: (libc::EPOLLIN | libc::EPOLLOUT | libc::EPOLLRDHUP) as u32,
            u64: idx as u64,
        };
        if unsafe { libc::epoll_ctl(self.ep, libc::EPOLL_CTL_ADD, fd, &mut ev) } < 0 {
            self.epoll_fail += 1;
            unsafe { libc::close(fd) };
            self.vus[idx].fd = -1;
            self.close_vu(idx, true);
            return;
        }
        self.schedule(idx, 30_000_000);
    }

    fn epoll_mod(&self, idx: usize, mut events: c_int) {
        let v = &self.vus[idx];
        if v.fd < 0 {
            return;
        }
        events |= libc::EPOLLRDHUP;
        if !v.out.is_empty() {
            events |= libc::EPOLLOUT;
        }
        let mut ev = libc::epoll_event {
            events: events as u32,
            u64: idx as u64,
        };
        unsafe {
            libc::epoll_ctl(self.ep, libc::EPOLL_CTL_MOD, v.fd, &mut ev);
        }
    }

    fn on_connect(&mut self, idx: usize) {
        let fd = self.vus[idx].fd;
        let mut err: c_int = 0;
        let mut len = mem::size_of::<c_int>() as libc::socklen_t;
        let rc = unsafe {
            libc::getsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_ERROR,
                &mut err as *mut _ as *mut c_void,
                &mut len,
            )
        };
        if rc < 0 || err != 0 {
            self.connect_fail += 1;
            self.close_vu(idx, true);
            return;
        }
        self.vus[idx].timer_gen = self.vus[idx].timer_gen.wrapping_add(1);
        self.vus[idx].state = State::Waiting;
        if !self.vus[idx].up {
            self.vus[idx].up = true;
            self.active += 1;
            self.connects += 1;
            if self.active > self.peak_active {
                self.peak_active = self.active;
            }
            if self.plateau_start.is_some() && self.active > self.min_active {
                // do not raise plateau minimum above target
            }
        }
        self.vus[idx].phase = 0;
        if self.vus[idx].started {
            self.send_next(idx);
        } else {
            self.vus[idx].started = true;
            self.vus[idx].state = State::Sleep;
            self.epoll_mod(idx, libc::EPOLLIN);
            let delay = self.rng.range(0, 5_000_000);
            self.schedule(idx, delay);
        }
    }

    fn send_next(&mut self, idx: usize) {
        // Phase 4 is the decision point after the post-reading think time.
        if self.vus[idx].phase == 4 {
            if self.rng.range(0, 100) < 15 {
                self.vus[idx].phase = 2;
            } else if self.rng.range(0, 100) < 2 {
                self.vus[idx].phase = 3;
            } else {
                self.vus[idx].phase = 0;
                self.vus[idx].iteration += 1;
                let delay = self.rng.range(5_000_000, 15_000_000);
                self.schedule(idx, delay);
                return;
            }
        }
        let v = &mut self.vus[idx];
        let user = &self.users[v.user];
        let host = &self.host_header;
        let mut s = String::with_capacity(256);
        match v.phase {
            0 => {
                s.push_str("GET /feed HTTP/1.1\r\nHost: ");
                s.push_str(host);
                s.push_str("\r\nConnection: keep-alive\r\n\r\n");
            }
            1 => {
                use std::fmt::Write;
                let _ = write!(s, "GET /posts/{} HTTP/1.1\r\nHost: {}\r\nConnection: keep-alive\r\n\r\n", v.post_id, host);
            }
            2 => {
                use std::fmt::Write;
                let _ = write!(
                    s,
                    "POST /posts/{}/like HTTP/1.1\r\nHost: {}\r\nAuthorization: Bearer {}\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n",
                    v.post_id, host, user.token
                );
            }
            _ => {
                use std::fmt::Write;
                let now = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH).expect("system clock");
                let secs = now.as_secs() as libc::time_t;
                let mut utc: libc::tm = unsafe { mem::zeroed() };
                assert!(!unsafe { libc::gmtime_r(&secs, &mut utc) }.is_null());
                let body = serde_json::json!({ "body": format!(
                    "{} says hi at {:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z (VU {}, iter {})",
                    user.name, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                    utc.tm_hour, utc.tm_min, utc.tm_sec, now.subsec_millis(),
                    self.vu_offset + idx + 1, v.iteration
                ) }).to_string();
                let _ = write!(
                    s,
                    "POST /posts HTTP/1.1\r\nHost: {}\r\nAuthorization: Bearer {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: keep-alive\r\n\r\n{}",
                    host, user.token, body.len(), body
                );
            }
        }
        v.out.clear();
        v.out.extend_from_slice(s.as_bytes());
        v.out_off = 0;
        v.lat_start = now_us();
        v.inflight = true;
        self.inflight_count += 1;
        v.state = State::Waiting;
        self.request_deadlines.push(Reverse((v.lat_start + 60_000_000, idx, v.lat_start)));
        self.flush_out(idx);
    }

    fn flush_out(&mut self, idx: usize) {
        loop {
            let v = &mut self.vus[idx];
            if v.out.is_empty() || v.out_off >= v.out.len() || v.fd < 0 {
                break;
            }
            let n = unsafe {
                libc::send(
                    v.fd,
                    v.out[v.out_off..].as_ptr() as *const c_void,
                    v.out.len() - v.out_off,
                    libc::MSG_NOSIGNAL,
                )
            };
            if n < 0 {
                let e = io::Error::last_os_error();
                if e.raw_os_error() == Some(libc::EAGAIN) {
                    break;
                }
                self.close_vu(idx, true);
                return;
            }
            if n == 0 {
                break;
            }
            v.out_off += n as usize;
        }
        if self.vus[idx].fd >= 0 {
            if self.vus[idx].out_off >= self.vus[idx].out.len() {
                self.vus[idx].out.clear();
                self.vus[idx].out_off = 0;
            }
            self.epoll_mod(idx, libc::EPOLLIN);
        }
    }

    fn parse_response(buf: &[u8]) -> Option<(u16, usize, usize)> {
        let hdr = buf.windows(4).position(|w| w == b"\r\n\r\n")? + 4;
        let head = &buf[..hdr];
        let mut it = head.split(|b| *b == b' ');
        it.next()?;
        let status = std::str::from_utf8(it.next()?).ok()?.parse::<u16>().ok()?;
        let mut clen = 0usize;
        for line in head.split(|b| *b == b'\n') {
            let line = line.strip_suffix(b"\r").unwrap_or(line);
            if line.len() > 15 && line[..15].eq_ignore_ascii_case(b"content-length:") {
                clen = std::str::from_utf8(&line[15..]).ok()?.trim().parse().ok()?;
            }
        }
        if buf.len() >= hdr + clen {
            Some((status, hdr, hdr + clen))
        } else {
            None
        }
    }

    fn complete(&mut self, idx: usize, status: u16, post_id: Option<i64>) {
        let phase = self.vus[idx].phase;
        self.vus[idx].inflight = false;
        self.inflight_count -= 1;
        let expected = match phase {
            0 => status == 200 && post_id.is_some(),
            1 => status == 200,
            2 => status == 200 || status == 201,
            _ => status == 201,
        };
        let started = self.vus[idx].lat_start;
        let in_plateau = self.plateau_start.map(|p| started >= p && started < self.plateau_end).unwrap_or(false);
        if in_plateau {
            self.requests += 1;
            if !expected {
                self.errors += 1;
            }
            let us = now_us().saturating_sub(started);
            self.phase_hist[phase.min(3) as usize][((us / 100_000) as usize).min(100)] += 1;
            self.max_us = self.max_us.max(us);
            let b = (us / 100) as usize;
            self.hist[b.min(NB - 1)] += 1;
        }
        self.total_requests += 1;
        let (delay, next) = match phase {
            0 => {
                if let Some(id) = post_id.filter(|_| expected) {
                    self.vus[idx].post_id = id;
                    (self.rng.range(3_000_000, 7_000_000), 1)
                } else {
                    (self.rng.range(3_000_000, 7_000_000), 0)
                }
            }
            1 => (self.rng.range(3_000_000, 8_000_000), 4),
            2 if self.rng.range(0, 100) < 2 => (0, 3),
            _ => (self.rng.range(5_000_000, 15_000_000), 0),
        };
        if next == 0 {
            self.vus[idx].iteration += 1;
        }
        self.vus[idx].phase = next;
        self.vus[idx].state = State::Sleep;
        self.schedule(idx, delay);
    }

    fn on_read(&mut self, idx: usize) -> bool {
        // recv fills exactly `n` bytes; zeroing 64 KiB per response is pure
        // client CPU and was the largest per-event cost in the load generator.
        let mut buf = [std::mem::MaybeUninit::<u8>::uninit(); READ_BUF];
        let fd = self.vus[idx].fd;
        let n = if idx % 256 == 0 {
            let mut iov = libc::iovec { iov_base: buf.as_mut_ptr() as *mut c_void, iov_len: READ_BUF };
            let mut control = [0u64; 8];
            let mut msg: libc::msghdr = unsafe { mem::zeroed() };
            msg.msg_iov = &mut iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control.as_mut_ptr() as *mut c_void;
            msg.msg_controllen = mem::size_of_val(&control);
            let n = unsafe { libc::recvmsg(fd, &mut msg, 0) };
            if n > 0 && self.plateau_start.is_some() && now_us() < self.plateau_end {
                let cmsg = unsafe { libc::CMSG_FIRSTHDR(&msg) };
                if !cmsg.is_null() && unsafe {
                    (*cmsg).cmsg_level == libc::SOL_SOCKET
                        && (*cmsg).cmsg_type == libc::SCM_TIMESTAMPNS
                        && (*cmsg).cmsg_len >= libc::CMSG_LEN(mem::size_of::<libc::timespec>() as u32) as usize
                } {
                    let ts = unsafe { std::ptr::read_unaligned(libc::CMSG_DATA(cmsg) as *const libc::timespec) };
                    if ts.tv_sec > 0 && ts.tv_nsec >= 0 {
                        let packet_us = (ts.tv_sec as u64) * 1_000_000 + (ts.tv_nsec as u64) / 1000;
                        if let Ok(now) = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
                            let lag_ms = (now.as_micros() as u64).saturating_sub(packet_us) / 1000;
                            self.packet_queue_lag_ms[(lag_ms as usize).min(1000)] += 1;
                        }
                    }
                }
            }
            n
        } else {
            unsafe { libc::recv(fd, buf.as_mut_ptr() as *mut c_void, READ_BUF, 0) }
        };
        if n < 0 {
            let e = io::Error::last_os_error();
            if e.raw_os_error() == Some(libc::EAGAIN) {
                return false;
            }
            self.close_vu(idx, true);
            return false;
        }
        if n == 0 {
            self.close_vu(idx, true);
            return false;
        }
        let bytes = unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, n as usize) };
        self.vus[idx].pending.extend_from_slice(bytes);
        if let Some((status, hdr, used)) = Self::parse_response(&self.vus[idx].pending) {
            let post_id = if self.vus[idx].phase == 0 && status == 200 {
                let json = serde_json::from_slice::<serde_json::Value>(
                    &self.vus[idx].pending[hdr..used],
                ).ok();
                json.as_ref()
                    .and_then(|value| value.get("posts"))
                    .and_then(|posts| posts.as_array())
                    .and_then(|posts| {
                        if posts.is_empty() {
                            None
                        } else {
                            posts[self.rng.range(0, posts.len() as u64) as usize]
                                .get("id")
                                .and_then(|id| id.as_i64())
                        }
                    })
            } else {
                None
            };
            self.vus[idx].pending.drain(..used);
            self.complete(idx, status, post_id);
        }
        true
    }

    fn on_event(&mut self, idx: usize, events: u32) {
        if idx >= self.vus.len() {
            return;
        }
        let closing = events & (libc::EPOLLERR | libc::EPOLLHUP | libc::EPOLLRDHUP) as u32 != 0;
        if self.vus[idx].state == State::Connecting {
            if events & libc::EPOLLOUT as u32 != 0 || closing {
                self.on_connect(idx);
            }
        } else {
            if !closing && events & libc::EPOLLOUT as u32 != 0 && !self.vus[idx].out.is_empty() {
                self.flush_out(idx);
            }
            if self.vus[idx].fd >= 0 && self.vus[idx].state == State::Waiting
                && events & libc::EPOLLIN as u32 != 0
            {
                while self.on_read(idx) && closing && self.vus[idx].fd >= 0
                    && self.vus[idx].state == State::Waiting {}
            }
        }
        if closing && self.vus[idx].fd >= 0 {
            self.close_vu(idx, true);
        }
    }

    fn timer(&mut self, idx: usize) {
        match self.vus[idx].state {
            State::Sleep => {
                if self.vus[idx].up {
                    self.send_next(idx);
                } else {
                    self.start_connect(idx);
                }
            }
            State::Retry => self.start_connect(idx),
            State::Connecting => {
                self.conn_timeouts += 1;
                self.close_vu(idx, true);
            }
            _ => {}
        }
    }

    fn pct(&self, q: u64) -> f64 {
        let total: u64 = self.hist.iter().sum();
        if total == 0 {
            return 0.0;
        }
        let target = (total * q + 99) / 100;
        let mut seen = 0u64;
        for (i, n) in self.hist.iter().enumerate() {
            seen += *n;
            if seen >= target {
                return (i as f64 + 1.0) / 10.0;
            }
        }
        NB as f64 / 10.0
    }

    fn lag_pct(hist: &[u64], q: u64) -> u64 {
        let total: u64 = hist.iter().sum();
        if total == 0 { return 0; }
        let target = (total * q + 99) / 100;
        let mut seen = 0;
        for (ms, count) in hist.iter().enumerate() {
            seen += count;
            if seen >= target { return ms as u64; }
        }
        (hist.len() - 1) as u64
    }

    fn run(&mut self, ramp_us: u64, hold_us: u64) {
        for i in 0..self.n {
            let at = if self.n > 1 {
                (i as u64).saturating_mul(ramp_us) / (self.n as u64 - 1)
            } else {
                0
            };
            self.heap.push(Reverse((at, i, self.vus[i].timer_gen)));
        }
        self.plateau_end = 0;
        let mut events = vec![unsafe { mem::zeroed::<libc::epoll_event>() }; EPOLL_EVENTS];
        let mut last_report = 0u64;
        let plateau_at = env::var("LG_PLATEAU_DELAY_S").ok().map(|s| {
            let seconds: f64 = s.parse().expect("LG_PLATEAU_DELAY_S");
            assert!(seconds.is_finite() && seconds >= 0.0);
            (seconds * 1e6) as u64
        });
        loop {
            let now = now_us();
            if self.plateau_start.is_none() && now >= self.connect_deadline {
                break;
            }
            let draining = self.plateau_start.is_some() && now >= self.plateau_end;
            if draining && (self.inflight_count == 0 || now >= self.plateau_end + 10_000_000) {
                break;
            }
            let start_plateau = match plateau_at {
                Some(at) => now >= at,
                None => self.active == self.n as i64,
            };
            if self.plateau_start.is_none() && start_plateau {
                let start = plateau_at.unwrap_or(now);
                self.plateau_start = Some(start);
                self.plateau_end = start + hold_us;
                self.min_active = self.active;
                self.peak_active = self.active;
                self.requests = 0;
                self.errors = 0;
                self.hist.fill(0);
                self.max_us = 0;
                println!("target-reached active={} at={:.2}s", self.active, now as f64 / 1e6);
            }
            if now >= last_report + 1_000_000 {
                last_report = now;
                if self.plateau_start.is_some() {
                    println!(
                        "t={:.0}s active={} reqs={} errs={}",
                        now as f64 / 1e6,
                        self.active,
                        self.requests,
                        self.errors
                    );
                } else {
                    println!(
                        "connect t={:.0}s active={}/{} ok={} bind_fail={} conn_fail={} epoll_fail={} timeouts={}",
                        now as f64 / 1e6,
                        self.active,
                        self.n,
                        self.connects,
                        self.bind_fail,
                        self.connect_fail,
                        self.epoll_fail,
                        self.conn_timeouts
                    );
                }
            }
            let mut next = if draining {
                self.plateau_end + 10_000_000
            } else {
                self.heap.peek().map(|Reverse((at, _, _))| *at).unwrap_or(now + 1_000_000)
            };
            if let Some(Reverse((at, _, _))) = self.request_deadlines.peek() {
                next = next.min(*at);
            }
            if let Some(at) = plateau_at {
                if self.plateau_start.is_none() {
                    next = next.min(at);
                }
            }
            if draining {
                next = next.min(now + 1_000_000);
            } else if self.plateau_start.is_some() {
                next = next.min(self.plateau_end);
            } else {
                next = next.min(self.connect_deadline);
            }
            let timeout = ((next.saturating_sub(now) + 999) / 1000).min(1000) as c_int;
            let n = unsafe {
                libc::epoll_wait(
                    self.ep,
                    events.as_mut_ptr(),
                    EPOLL_EVENTS as c_int,
                    timeout,
                )
            };
            let batch_at = now_us();
            if n > 0 {
                for ev in &events[..n as usize] {
                    let idx = ev.u64 as usize;
                    if self.plateau_start.is_some() && !draining
                        && ev.events & libc::EPOLLIN as u32 != 0 {
                        let lag = now_us().saturating_sub(batch_at) / 1000;
                        self.read_batch_lag_ms[(lag as usize).min(1000)] += 1;
                    }
                    if !draining || (idx < self.vus.len() && self.vus[idx].state == State::Waiting) {
                        self.on_event(idx, ev.events);
                    }
                }
            }
            let now = now_us();
            while let Some(Reverse((at, idx, started))) = self.request_deadlines.peek().copied() {
                if at > now { break; }
                self.request_deadlines.pop();
                if self.vus[idx].inflight && self.vus[idx].lat_start == started {
                    self.request_timeouts += 1;
                    self.close_vu(idx, true);
                }
            }
            let mut timers = 0usize;
            while !draining {
                let Some(Reverse((at, idx, generation))) = self.heap.peek().copied() else { break };
                if at > now || timers >= 1024
                    || (self.plateau_start.is_some() && now_us() >= self.plateau_end) {
                    break;
                }
                self.heap.pop();
                if self.vus[idx].timer_gen != generation {
                    continue;
                }
                if self.plateau_start.is_some() {
                    let lag = now_us().saturating_sub(at) / 1000;
                    self.timer_lag_ms[(lag as usize).min(1000)] += 1;
                }
                self.timer(idx);
                timers += 1;
            }
        }
        // Requests still in flight after the ten-second drain are failures;
        // their unknown final duration is not a completed-latency sample.
        if let Some(start) = self.plateau_start {
            for v in &self.vus {
                if v.inflight && v.lat_start >= start && v.lat_start < self.plateau_end {
                    self.requests += 1;
                    self.errors += 1;
                    self.unfinished += 1;
                }
            }
        }
        // Release sockets gradually after the measured hold and request drain.
        // An instantaneous close of every VU creates an artificial FIN burst.
        let rampdown_us = env::var("LG_RAMPDOWN_S").ok()
            .map(|s| s.parse::<u64>().expect("LG_RAMPDOWN_S").saturating_mul(1_000_000))
            .unwrap_or(0);
        let rampdown_start = Instant::now();
        for i in 0..self.n {
            self.close_vu(i, false);
            if rampdown_us != 0 && (i % 32 == 31 || i + 1 == self.n) {
                let due = rampdown_us.saturating_mul((i + 1) as u64) / self.n as u64;
                if let Some(delay) = std::time::Duration::from_micros(due).checked_sub(rampdown_start.elapsed()) {
                    std::thread::sleep(delay);
                }
            }
        }
    }
}

fn parse_tokens(path: &str) -> Result<Vec<User>, String> {
    let raw = fs::read_to_string(path).map_err(|e| format!("tokens: {e}"))?;
    let val: serde_json::Value =
        serde_json::from_str(&raw).map_err(|e| format!("tokens json: {e}"))?;
    let arr = val.as_array().ok_or("tokens must be an array")?;
    let mut out = Vec::with_capacity(arr.len());
    for v in arr {
        let token = v.get("token").and_then(|x| x.as_str()).ok_or("missing token")?;
        let name = v
            .get("username")
            .and_then(|x| x.as_str())
            .ok_or("missing username")?;
        out.push(User {
            token: token.to_string(),
            name: name.to_string(),
        });
    }
    if out.is_empty() {
        return Err("no tokens".into());
    }
    Ok(out)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 7 {
        eprintln!("usage: {} HOST PORT USERS RAMP_S HOLD_S TOKENS [NSRC] [CONNECT_TIMEOUT_S] [SRC_BASE]", args[0]);
        std::process::exit(2);
    }
    let host = &args[1];
    let port: u16 = args[2].parse().expect("port");
    let n: usize = args[3].parse().expect("users");
    let ramp_s: f64 = args[4].parse().expect("ramp");
    let hold_s: f64 = args[5].parse().expect("hold");
    let tokens_path = &args[6];
    let nsrc: usize = args.get(7).and_then(|x| x.parse().ok()).unwrap_or(25).min(250);
    let connect_timeout_s: f64 = args.get(8).and_then(|x| x.parse().ok()).unwrap_or(600.0);
    let src_base: usize = args.get(9).and_then(|x| x.parse().ok()).unwrap_or(0);
    let vu_offset: usize = env::var("LG_VU_OFFSET").ok()
        .map(|value| value.parse().expect("LG_VU_OFFSET")).unwrap_or(0);
    if nsrc > 250 || src_base + nsrc > 250 {
        eprintln!("nsrc must be 0..250 and src_base+nsrc <= 250");
        std::process::exit(2);
    }
    let users = parse_tokens(tokens_path).unwrap_or_else(|e| {
        eprintln!("{e}");
        std::process::exit(2);
    });
    let srv_ip: Ipv4Addr = host.parse().expect("IPv4 host");
    let mut srv: libc::sockaddr_in = unsafe { mem::zeroed() };
    srv.sin_family = libc::AF_INET as u16;
    srv.sin_port = port.to_be();
    srv.sin_addr.s_addr = u32::from_ne_bytes(srv_ip.octets());
    let ep = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    if ep < 0 {
        eprintln!("epoll_create1: {}", io::Error::last_os_error());
        std::process::exit(1);
    }
    let start = if let Ok(value) = env::var("LG_START_UNIX_MS") {
        let target: u64 = value.parse().expect("LG_START_UNIX_MS");
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).expect("system clock").as_millis() as u64;
        assert!(target > now, "LG_START_UNIX_MS must be in the future");
        let delay = std::time::Duration::from_millis(target - now);
        let start = Instant::now() + delay;
        std::thread::sleep(delay);
        start
    } else {
        Instant::now()
    };
    let _ = START.set(start);
    let mut vus = Vec::with_capacity(n);
    for i in 0..n {
        vus.push(Vu::new((vu_offset + i) % users.len()));
    }
    let mut ctx = Ctx {
        vus,
        users,
        vu_offset,
        heap: BinaryHeap::with_capacity(n),
        request_deadlines: BinaryHeap::new(),
        ep,
        srv,
        host_header: format!("{}:{}", host, port),
        nsrc,
        src_base,
        n,
        rng: Rng(0x9E3779B97F4A7C15 ^ (src_base as u64).wrapping_mul(0xBF58476D1CE4E5B9)),
        active: 0,
        connects: 0,
        bind_fail: 0,
        connect_fail: 0,
        epoll_fail: 0,
        conn_timeouts: 0,
        peak_active: 0,
        min_active: 0,
        plateau_start: None,
        plateau_end: 0,
        connect_deadline: (connect_timeout_s * 1e6) as u64,
        requests: 0,
        errors: 0,
        inflight_count: 0,
        unfinished: 0,
        request_timeouts: 0,
        total_requests: 0,
        hist: vec![0; NB],
        phase_hist: [[0; 101]; 4],
        timer_lag_ms: vec![0; 1001],
        read_batch_lag_ms: vec![0; 1001],
        packet_queue_lag_ms: vec![0; 1001],
        max_us: 0,
    };
    println!(
        "target={} ramp={}s hold={}s tokens={} nsrc={} connect_timeout={}s src_base={}",
        n, ramp_s, hold_s, ctx.users.len(), nsrc, connect_timeout_s, src_base
    );
    ctx.run((ramp_s * 1e6) as u64, (hold_s * 1e6) as u64);
    let reached = ctx.plateau_start.is_some();
    let held = reached && ctx.min_active >= n as i64;
    let p95 = ctx.pct(95);
    let p99 = ctx.pct(99);
    let p50 = ctx.pct(50);
    println!(
        "result reached={} target={} peak_active={} min_active={} plateau_requests={} errors={} unfinished={} request_timeouts={}",
        reached, n, ctx.peak_active, ctx.min_active, ctx.requests, ctx.errors, ctx.unfinished, ctx.request_timeouts
    );
    println!(
        "p50={:.1}ms p95={:.1}ms p99={:.1}ms max={:.1}ms total_reqs={}",
        p50,
        p95,
        p99,
        ctx.max_us as f64 / 1000.0,
        ctx.total_requests
    );
    println!(
        "client timer_lag_p99_ms={} read_batch_lag_p99_ms={} packet_queue_lag_p99_ms={} packet_samples={} (1000 means >=1000)",
        Ctx::lag_pct(&ctx.timer_lag_ms, 99),
        Ctx::lag_pct(&ctx.read_batch_lag_ms, 99),
        Ctx::lag_pct(&ctx.packet_queue_lag_ms, 99),
        ctx.packet_queue_lag_ms.iter().sum::<u64>()
    );
    for (phase, name) in ["feed", "post", "like", "create"].iter().enumerate() {
        let hist = &ctx.phase_hist[phase];
        println!("phase={} count={} p95_ms_upper={} (10100 means >=10000)",
            name, hist.iter().sum::<u64>(), (Ctx::lag_pct(hist, 95) + 1) * 100);
    }
    if let Ok(path) = env::var("LG_HISTOGRAM_PATH") {
        let histogram = serde_json::json!({
            "target": n,
            "min_active": ctx.min_active,
            "reached": reached,
            "requests": ctx.requests,
            "errors": ctx.errors,
            "unfinished": ctx.unfinished,
            "request_timeouts": ctx.request_timeouts,
            "max_us": ctx.max_us,
            "timer_lag_p99_ms": Ctx::lag_pct(&ctx.timer_lag_ms, 99),
            "read_batch_lag_p99_ms": Ctx::lag_pct(&ctx.read_batch_lag_ms, 99),
            "packet_queue_lag_p99_ms": Ctx::lag_pct(&ctx.packet_queue_lag_ms, 99),
            "packet_queue_samples": ctx.packet_queue_lag_ms.iter().sum::<u64>(),
            "histogram_100us": ctx.hist,
        });
        fs::write(path, histogram.to_string()).expect("writing histogram");
    }
    let err_pct = if ctx.requests > 0 {
        ctx.errors as f64 * 100.0 / ctx.requests as f64
    } else {
        100.0
    };
    let pass = reached && held && ctx.requests > 0 && p95 < 500.0 && p99 < 1000.0 && err_pct < 1.0;
    println!(
        "{} active_target_held={} p95<500={} p99<1000={} err<1%={}",
        if pass { "PASS" } else { "FAIL" },
        held,
        p95 < 500.0,
        p99 < 1000.0,
        err_pct < 1.0
    );
    std::process::exit(if pass { 0 } else { 99 });
}
