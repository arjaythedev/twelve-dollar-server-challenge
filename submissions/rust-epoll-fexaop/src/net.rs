//! Linux epoll reactor. Parsing, auth and response framing run here; all
//! SQLite work is submitted to the ordered worker and returned over a channel.
use crate::{Job, Op, Reply, error, lufp};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::{
    borrow::Cow,
    collections::VecDeque,
    io,
    net::SocketAddr,
    sync::mpsc::{Receiver, Sender},
    time::{Instant, SystemTime, UNIX_EPOCH},
};

const MAX_FD: usize = 65536;
const MAX_INPUT: usize = 1_114_112;
const MAX_OUTPUT: usize = 1 << 20;
const MAX_BATCH_PER_READ: usize = 64;
const OUTSTANDING_CAP: u16 = 8;
const WAKE_TAG: u64 = u64::MAX;
const IN: u32 = (libc::EPOLLIN | libc::EPOLLRDHUP) as u32;

struct Auth {
    mac: Hmac<Sha256>,
    header: Vec<u8>,
    payload: Vec<u8>,
}

impl Auth {
    fn new(secret: &str) -> Self {
        Self {
            mac: Hmac::<Sha256>::new_from_slice(secret.as_bytes()).unwrap(),
            header: Vec::with_capacity(256),
            payload: Vec::with_capacity(256),
        }
    }

    fn user(&mut self, authorization: Option<&str>) -> Result<(i64, Cow<'_, str>), &'static str> {
        let token = authorization
            .and_then(|a| a.strip_prefix("Bearer "))
            .ok_or("missing bearer token")?;
        let bad = "invalid or expired token";
        if token.len() > 16384 {
            return Err(bad);
        }
        let mut pieces = token.split('.');
        let h = pieces.next().ok_or(bad)?;
        let p = pieces.next().ok_or(bad)?;
        let s = pieces.next().ok_or(bad)?;
        if pieces.next().is_some() {
            return Err(bad);
        }
        let mut sig = [0u8; 33];
        let n = URL_SAFE_NO_PAD.decode_slice(s, &mut sig).map_err(|_| bad)?;
        let mut mac = self.mac.clone();
        mac.update(&token.as_bytes()[..h.len() + 1 + p.len()]);
        mac.verify_slice(&sig[..n]).map_err(|_| bad)?;
        self.header.clear();
        URL_SAFE_NO_PAD
            .decode_vec(h, &mut self.header)
            .map_err(|_| bad)?;
        lufp::header(&self.header).map_err(|_| bad)?;
        self.payload.clear();
        URL_SAFE_NO_PAD
            .decode_vec(p, &mut self.payload)
            .map_err(|_| bad)?;
        let claims = lufp::claims(&self.payload).map_err(|_| bad)?;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs_f64();
        let exp = claims.exp.ok_or(bad)?;
        if exp <= now || claims.nbf.is_some_and(|n| n > now) {
            return Err(bad);
        }
        let payload_error = "invalid token payload";
        let sub = claims.sub.ok_or(payload_error)?;
        let id = positive(&sub).ok_or(payload_error)?;
        let name = claims.username.ok_or(payload_error)?;
        Ok((id, name))
    }
}

pub struct Frame<'a> {
    method: &'a str,
    path: &'a str,
    authorization: Option<&'a str>,
    body: Cow<'a, [u8]>,
    pub consumed: usize,
    pub close: bool,
}

pub enum Parsed<'a> {
    Incomplete { expect_continue: bool },
    Complete(Frame<'a>),
}

pub fn frame(input: &[u8]) -> Result<Parsed<'_>, ()> {
    let mut headers = [httparse::EMPTY_HEADER; 64];
    let mut req = httparse::Request::new(&mut headers);
    let end = match req.parse(input).map_err(|_| ())? {
        httparse::Status::Partial => {
            return if input.len() > 65536 {
                Err(())
            } else {
                Ok(Parsed::Incomplete {
                    expect_continue: false,
                })
            };
        }
        httparse::Status::Complete(n) => n,
    };
    let mut authorization = None;
    let mut length = None;
    let mut chunked = false;
    let mut expect_continue = false;
    let mut close = req.version != Some(1);
    for h in req.headers.iter() {
        if h.name.eq_ignore_ascii_case("authorization") {
            if authorization.is_some() {
                return Err(());
            }
            authorization = Some(std::str::from_utf8(h.value).map_err(|_| ())?);
        } else if h.name.eq_ignore_ascii_case("content-length") {
            if length.is_some() {
                return Err(());
            }
            let s = std::str::from_utf8(h.value).map_err(|_| ())?.trim();
            if s.is_empty() || !s.bytes().all(|c| c.is_ascii_digit()) {
                return Err(());
            }
            length = Some(s.parse::<usize>().map_err(|_| ())?);
        } else if h.name.eq_ignore_ascii_case("transfer-encoding") {
            if chunked || !h.value.eq_ignore_ascii_case(b"chunked") {
                return Err(());
            }
            chunked = true;
        } else if h.name.eq_ignore_ascii_case("connection") {
            close |= h
                .value
                .split(|&c| c == b',')
                .any(|v| v.trim_ascii().eq_ignore_ascii_case(b"close"));
        } else if h.name.eq_ignore_ascii_case("expect") {
            if !h.value.eq_ignore_ascii_case(b"100-continue") {
                return Err(());
            }
            expect_continue = req.version == Some(1);
        }
    }
    if end > 65536 || length.is_some_and(|n| n > 1_048_576) {
        return Err(());
    }
    let (body, consumed) = if chunked {
        if length.is_some() {
            return Err(());
        }
        let Some((body, n)) = chunks(&input[end..])? else {
            return Ok(Parsed::Incomplete { expect_continue });
        };
        (Cow::Owned(body), end + n)
    } else {
        let length = length.unwrap_or(0);
        if input.len() < end + length {
            return Ok(Parsed::Incomplete { expect_continue });
        }
        (Cow::Borrowed(&input[end..end + length]), end + length)
    };
    Ok(Parsed::Complete(Frame {
        method: req.method.ok_or(())?,
        path: req.path.ok_or(())?,
        authorization,
        body,
        consumed,
        close,
    }))
}

fn chunks(input: &[u8]) -> Result<Option<(Vec<u8>, usize)>, ()> {
    let mut i = 0;
    let mut output = Vec::new();
    loop {
        let Some(n) = memchr::memmem::find(&input[i..], b"\r\n") else {
            return Ok(None);
        };
        let size = input[i..i + n].split(|&c| c == b';').next().ok_or(())?;
        if size.is_empty() || size.len() > 8 || !size.iter().all(|c| c.is_ascii_hexdigit()) {
            return Err(());
        }
        let size = usize::from_str_radix(std::str::from_utf8(size).map_err(|_| ())?, 16)
            .map_err(|_| ())?;
        i += n + 2;
        if size == 0 {
            loop {
                let Some(n) = memchr::memmem::find(&input[i..], b"\r\n") else {
                    return Ok(None);
                };
                i += n + 2;
                if n == 0 {
                    return Ok(Some((output, i)));
                }
                if i > 65536 + output.len() {
                    return Err(());
                }
            }
        }
        if size > 1_048_576 - output.len() {
            return Err(());
        }
        if input.len() < i + size + 2 {
            return Ok(None);
        }
        if &input[i + size..i + size + 2] != b"\r\n" {
            return Err(());
        }
        output.extend_from_slice(&input[i..i + size]);
        i += size + 2;
    }
}

fn positive(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    s.parse::<i64>().ok().filter(|v| *v > 0)
}

#[cold]
fn post_error(path: &str, status: u16, message: &'static str) -> (u16, &'static str) {
    if path
        .strip_prefix("/posts/")
        .and_then(|s| s.strip_suffix("/like"))
        .is_some_and(|s| s.contains('/'))
    {
        return (404, "not found");
    }
    (status, message)
}

fn route(auth: &mut Auth, req: &Frame<'_>) -> Result<Op, (u16, &'static str)> {
    let path = req.path.split('?').next().unwrap_or(req.path);
    if req.method == "GET" {
        if path == "/feed" {
            return Ok(Op::Read(None));
        }
        if path == "/health" {
            return Ok(Op::Health);
        }
        if let Some(s) = path.strip_prefix("/posts/") {
            if let Some(id) = positive(s) {
                return Ok(Op::Read(Some(id)));
            }
            if !s.contains('/') {
                return Err((400, "invalid post id"));
            }
        }
    }
    if req.method == "POST"
        && (path == "/posts" || (path.starts_with("/posts/") && path.ends_with("/like")))
    {
        let (user, username) = match auth.user(req.authorization) {
            Ok(v) => v,
            Err(msg) => return Err(post_error(path, 401, msg)),
        };
        if path == "/posts" {
            let value = lufp::post_body(&req.body).map_err(|_| (400, "malformed JSON body"))?;
            let Some(body) = value else {
                return Err((400, "body is required"));
            };
            let body = body.trim_matches(|c: char| c.is_whitespace() || c == '\u{feff}');
            if body.is_empty() {
                return Err((400, "body is required"));
            }
            if body.chars().count() > 500 {
                return Err((400, "body must be at most 500 characters"));
            }
            return Ok(Op::Create {
                user,
                username: username.into_owned().into_boxed_str(),
                body: Box::from(body),
            });
        }
        let Some(id) = path
            .strip_prefix("/posts/")
            .and_then(|s| s.strip_suffix("/like"))
            .and_then(positive)
        else {
            return Err(post_error(path, 400, "invalid post id"));
        };
        return Ok(Op::Like { user, post: id });
    }
    Err((404, "not found"))
}

#[derive(Default)]
struct Client {
    live: bool,
    generation: u64,
    input: Vec<u8>,
    output: Vec<u8>,
    sent: usize,
    events: u32,
    ready: bool,
    paused: bool,
    closing: bool,
    close_when_flushed: bool,
    peer_closed: bool,
    continued: bool,
    outstanding: u16,
    touched: u64,
}

struct Outcome {
    consumed: usize,
    closing: bool,
}

struct Reactor {
    ep: i32,
    listener: i32,
    reactor: u16,
    jobs: Sender<Job>,
    auth: Auth,
    clients: Vec<Client>,
    ready: VecDeque<usize>,
    start: Instant,
    next_gen: u64,
    last_sweep: u64,
}

fn ctl(ep: i32, op: i32, fd: i32, flags: u32) -> io::Result<()> {
    let mut ev = libc::epoll_event {
        events: flags,
        u64: fd as u64,
    };
    if unsafe { libc::epoll_ctl(ep, op, fd, &mut ev) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn bind_reuseport(address: SocketAddr) -> io::Result<i32> {
    unsafe {
        let (domain, storage, len) = match address {
            SocketAddr::V4(a) => {
                let sa = libc::sockaddr_in {
                    sin_family: libc::AF_INET as u16,
                    sin_port: a.port().to_be(),
                    sin_addr: libc::in_addr {
                        s_addr: u32::from_ne_bytes(a.ip().octets()),
                    },
                    sin_zero: [0; 8],
                };
                (
                    libc::AF_INET,
                    SaStorage { v4: sa },
                    std::mem::size_of::<libc::sockaddr_in>() as u32,
                )
            }
            SocketAddr::V6(a) => {
                let sa = libc::sockaddr_in6 {
                    sin6_family: libc::AF_INET6 as u16,
                    sin6_port: a.port().to_be(),
                    sin6_flowinfo: 0,
                    sin6_addr: libc::in6_addr {
                        s6_addr: a.ip().octets(),
                    },
                    sin6_scope_id: a.scope_id(),
                };
                (
                    libc::AF_INET6,
                    SaStorage { v6: sa },
                    std::mem::size_of::<libc::sockaddr_in6>() as u32,
                )
            }
        };
        let fd = libc::socket(
            domain,
            libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            0,
        );
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let one: i32 = 1;
        for opt in [libc::SO_REUSEADDR, libc::SO_REUSEPORT] {
            if libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                opt,
                (&one as *const i32).cast(),
                std::mem::size_of::<i32>() as u32,
            ) != 0
            {
                let err = io::Error::last_os_error();
                libc::close(fd);
                return Err(err);
            }
        }
        let rc = libc::bind(
            fd,
            (&storage as *const SaStorage).cast(),
            len,
        );
        if rc != 0 {
            let err = io::Error::last_os_error();
            libc::close(fd);
            return Err(err);
        }
        if libc::listen(fd, 4096) != 0 {
            let err = io::Error::last_os_error();
            libc::close(fd);
            return Err(err);
        }
        Ok(fd)
    }
}

union SaStorage {
    v4: libc::sockaddr_in,
    v6: libc::sockaddr_in6,
}

fn desired(c: &Client) -> u32 {
    if !c.live {
        return 0;
    }
    if c.output.len() > c.sent {
        return libc::EPOLLOUT as u32;
    }
    if c.closing || c.paused || c.peer_closed {
        return 0;
    }
    IN
}

fn recv_some(fd: usize, buf: &mut [u8]) -> Result<Option<usize>, ()> {
    loop {
        let n = unsafe { libc::recv(fd as i32, buf.as_mut_ptr().cast(), buf.len(), 0) };
        if n >= 0 {
            return Ok(Some(n as usize));
        }
        match io::Error::last_os_error().raw_os_error() {
            Some(libc::EAGAIN) => return Ok(None),
            Some(libc::EINTR) => continue,
            _ => return Err(()),
        }
    }
}

fn send_some(fd: usize, data: &[u8]) -> Result<usize, ()> {
    loop {
        let n = unsafe {
            libc::send(
                fd as i32,
                data.as_ptr().cast(),
                data.len(),
                libc::MSG_NOSIGNAL,
            )
        };
        if n >= 0 {
            return Ok(n as usize);
        }
        match io::Error::last_os_error().raw_os_error() {
            Some(libc::EAGAIN) => return Ok(0),
            Some(libc::EINTR) => continue,
            _ => return Err(()),
        }
    }
}

fn send_iov(fd: usize, a: &[u8], b: &[u8]) -> Result<usize, ()> {
    let iov = [
        libc::iovec {
            iov_base: a.as_ptr().cast_mut().cast(),
            iov_len: a.len(),
        },
        libc::iovec {
            iov_base: b.as_ptr().cast_mut().cast(),
            iov_len: b.len(),
        },
    ];
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = iov.as_ptr().cast_mut();
    msg.msg_iovlen = iov.len();
    loop {
        let n = unsafe { libc::sendmsg(fd as i32, &msg, libc::MSG_NOSIGNAL) };
        if n >= 0 {
            return Ok(n as usize);
        }
        match io::Error::last_os_error().raw_os_error() {
            Some(libc::EAGAIN) => return Ok(0),
            Some(libc::EINTR) => continue,
            _ => return Err(()),
        }
    }
}

fn build_head(status: u16, len: usize, close: bool, head: &mut [u8; 128]) -> usize {
    fn put(head: &mut [u8; 128], at: &mut usize, bytes: &[u8]) {
        head[*at..*at + bytes.len()].copy_from_slice(bytes);
        *at += bytes.len();
    }
    let line: &[u8] = match status {
        200 => b"HTTP/1.1 200 OK\r\n",
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    };
    let mut at = 0;
    put(head, &mut at, line);
    put(head, &mut at, b"Content-Type: application/json\r\nContent-Length: ");
    let mut buf = itoa::Buffer::new();
    put(head, &mut at, buf.format(len).as_bytes());
    if close {
        put(head, &mut at, b"\r\nConnection: close");
    }
    put(head, &mut at, b"\r\n\r\n");
    at
}

fn append_unsent(c: &mut Client, a: &[u8], b: &[u8], sent: usize) {
    if sent < a.len() {
        c.output.extend_from_slice(&a[sent..]);
        c.output.extend_from_slice(b);
    } else {
        c.output.extend_from_slice(&b[sent - a.len()..]);
    }
    c.sent = 0;
}

impl Reactor {
    fn new(
        reactor: u16,
        address: &str,
        secret: &str,
        jobs: Sender<Job>,
        wake: i32,
    ) -> io::Result<Self> {
        let address: SocketAddr = address
            .parse()
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
        let listener = bind_reuseport(address)?;
        let ep = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        if ep < 0 {
            unsafe {
                libc::close(listener);
            }
            return Err(io::Error::last_os_error());
        }
        ctl(ep, libc::EPOLL_CTL_ADD, listener, libc::EPOLLIN as u32)?;
        let mut ev = libc::epoll_event {
            events: libc::EPOLLIN as u32,
            u64: WAKE_TAG,
        };
        if unsafe { libc::epoll_ctl(ep, libc::EPOLL_CTL_ADD, wake, &mut ev) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let clients = (0..MAX_FD).map(|_| Client::default()).collect();
        Ok(Self {
            ep,
            listener,
            reactor,
            jobs,
            auth: Auth::new(secret),
            clients,
            ready: VecDeque::with_capacity(1024),
            start: Instant::now(),
            next_gen: 1,
            last_sweep: 0,
        })
    }

    fn close(&mut self, fd: usize) {
        let c = &mut self.clients[fd];
        if !c.live {
            return;
        }
        unsafe {
            libc::epoll_ctl(self.ep, libc::EPOLL_CTL_DEL, fd as i32, std::ptr::null_mut());
            libc::close(fd as i32);
        }
        let generation = c.generation;
        *c = Client {
            generation,
            ..Default::default()
        };
    }

    fn sync(&mut self, fd: usize) -> bool {
        let want = desired(&self.clients[fd]);
        if self.clients[fd].events == want {
            return self.clients[fd].live;
        }
        let mut ev = libc::epoll_event {
            events: want,
            u64: fd as u64,
        };
        if unsafe { libc::epoll_ctl(self.ep, libc::EPOLL_CTL_MOD, fd as i32, &mut ev) } != 0 {
            self.close(fd);
            return false;
        }
        self.clients[fd].events = want;
        true
    }

    fn accept(&mut self, now: u64) {
        for _ in 0..64 {
            let fd = unsafe {
                libc::accept4(
                    self.listener,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                )
            };
            if fd < 0 {
                break;
            }
            let idx = fd as usize;
            if idx >= MAX_FD || idx == self.listener as usize {
                unsafe {
                    libc::close(fd);
                }
                continue;
            }
            let enabled: i32 = 1;
            unsafe {
                libc::setsockopt(
                    fd,
                    libc::IPPROTO_TCP,
                    libc::TCP_NODELAY,
                    (&enabled as *const i32).cast(),
                    std::mem::size_of::<i32>() as u32,
                );
            }
            let generation = self.next_gen;
            self.next_gen = self.next_gen.wrapping_add(1);
            self.clients[idx] = Client {
                live: true,
                generation,
                touched: now,
                events: IN,
                ..Default::default()
            };
            if ctl(self.ep, libc::EPOLL_CTL_ADD, fd, IN).is_err() {
                self.close(idx);
            }
        }
    }

    fn enqueue(&mut self, fd: usize, close: bool, op: Op) -> bool {
        let job = Job {
            reactor: self.reactor,
            fd: fd as u32,
            generation: self.clients[fd].generation,
            close,
            op,
        };
        if self.jobs.send(job).is_ok() {
            let c = &mut self.clients[fd];
            c.outstanding += 1;
            if c.outstanding >= OUTSTANDING_CAP {
                c.paused = true;
            }
            true
        } else {
            self.clients[fd].closing = true;
            false
        }
    }

    fn parse(&mut self, fd: usize, bytes: &[u8]) -> Outcome {
        let mut offset = 0;
        let mut count = 0;
        let mut closing = false;
        while offset < bytes.len() {
            if self.clients[fd].paused {
                break;
            }
            match frame(&bytes[offset..]) {
                Ok(Parsed::Complete(req)) => {
                    self.clients[fd].continued = false;
                    let op = match route(&mut self.auth, &req) {
                        Ok(op) => op,
                        Err((status, msg)) => {
                            let mut body = Vec::with_capacity(48);
                            let status = error(&mut body, status, msg);
                            Op::Immediate { status, body }
                        }
                    };
                    let close = req.close;
                    if !self.enqueue(fd, close, op) {
                        closing = true;
                        break;
                    }
                    offset += req.consumed;
                    count += 1;
                    if close {
                        closing = true;
                        break;
                    }
                    if count >= MAX_BATCH_PER_READ {
                        break;
                    }
                }
                Ok(Parsed::Incomplete { expect_continue }) => {
                    if expect_continue && !self.clients[fd].continued {
                        if !self.enqueue(fd, false, Op::Continue) {
                            closing = true;
                        }
                        self.clients[fd].continued = true;
                    }
                    break;
                }
                Err(()) => {
                    let mut body = Vec::with_capacity(48);
                    let status = error(&mut body, 400, "malformed request");
                    let _ = self.enqueue(fd, true, Op::Immediate { status, body });
                    closing = true;
                    break;
                }
            }
        }
        Outcome {
            consumed: offset,
            closing,
        }
    }

    fn handle_read(&mut self, fd: usize, scratch: &mut [u8], now: u64) -> bool {
        {
            let c = &self.clients[fd];
            if !c.live || c.paused || c.closing || c.peer_closed {
                return c.live;
            }
            if c.output.len() > c.sent {
                return true;
            }
        }
        let n = match recv_some(fd, scratch) {
            Ok(Some(n)) => n,
            Ok(None) => return true,
            Err(()) => {
                self.close(fd);
                return false;
            }
        };
        if n == 0 {
            let mut close_now = false;
            {
                let c = &mut self.clients[fd];
                c.peer_closed = true;
                if c.outstanding == 0 && c.output.len() == c.sent {
                    close_now = true;
                }
            }
            if close_now {
                self.close(fd);
                return false;
            }
            self.sync(fd);
            return true;
        }
        self.clients[fd].touched = now;

        let mut closing = false;
        if self.clients[fd].input.is_empty() {
            let out = self.parse(fd, &scratch[..n]);
            let c = &mut self.clients[fd];
            if out.closing {
                c.closing = true;
                c.input.clear();
                closing = true;
            } else if out.consumed < n {
                c.input.extend_from_slice(&scratch[out.consumed..n]);
                if !c.paused && out.consumed > 0 {
                    c.ready = true;
                }
            }
        } else {
            let mut input = std::mem::take(&mut self.clients[fd].input);
            input.extend_from_slice(&scratch[..n]);
            if input.len() > MAX_INPUT {
                let mut body = Vec::with_capacity(48);
                let status = error(&mut body, 400, "malformed request");
                let _ = self.enqueue(fd, true, Op::Immediate { status, body });
                self.clients[fd].closing = true;
                closing = true;
            } else {
                let out = self.parse(fd, &input);
                input.drain(..out.consumed);
                let c = &mut self.clients[fd];
                if out.closing {
                    c.closing = true;
                    c.input.clear();
                    closing = true;
                } else {
                    let more = !input.is_empty() && !c.paused && out.consumed > 0;
                    c.input = input;
                    if more {
                        c.ready = true;
                    }
                }
            }
        }
        if !closing && self.clients[fd].ready {
            self.ready.push_back(fd);
        }
        self.sync(fd);
        self.clients[fd].live
    }

    fn process_ready(&mut self) {
        let queued = self.ready.len().min(256);
        for _ in 0..queued {
            let Some(fd) = self.ready.pop_front() else {
                break;
            };
            if fd >= MAX_FD || !self.clients[fd].live {
                continue;
            }
            if !self.clients[fd].ready {
                continue;
            }
            self.clients[fd].ready = false;
            if self.clients[fd].paused || self.clients[fd].closing || self.clients[fd].peer_closed {
                continue;
            }
            if self.clients[fd].input.is_empty() {
                continue;
            }
            let mut input = std::mem::take(&mut self.clients[fd].input);
            let out = self.parse(fd, &input);
            input.drain(..out.consumed);
            let mut requeue = false;
            {
                let c = &mut self.clients[fd];
                if out.closing {
                    c.closing = true;
                    c.input.clear();
                } else {
                    requeue = !input.is_empty() && !c.paused && out.consumed > 0;
                    c.input = input;
                    if requeue {
                        c.ready = true;
                    }
                }
            }
            if requeue {
                self.ready.push_back(fd);
            }
            self.sync(fd);
        }
    }

    fn flush(&mut self, fd: usize, now: u64) -> bool {
        let mut close_now = false;
        let mut requeue = false;
        {
            let c = &mut self.clients[fd];
            if !c.live {
                return false;
            }
            if c.sent < c.output.len() {
                match send_some(fd, &c.output[c.sent..]) {
                    Ok(n) => {
                        c.sent += n;
                        c.touched = now;
                    }
                    Err(()) => close_now = true,
                }
            }
            if !close_now && c.sent >= c.output.len() {
                c.output.clear();
                c.sent = 0;
                if c.output.capacity() > 65536 {
                    c.output = Vec::new();
                }
                if c.close_when_flushed {
                    close_now = true;
                } else if c.peer_closed && c.outstanding == 0 {
                    close_now = true;
                } else if !c.paused && !c.closing && !c.input.is_empty() {
                    c.ready = true;
                    requeue = true;
                }
            }
        }
        if close_now {
            self.close(fd);
            return false;
        }
        if requeue {
            self.ready.push_back(fd);
        }
        self.sync(fd);
        true
    }

    fn deliver(&mut self, reply: Reply, now: u64) {
        let fd = reply.fd as usize;
        if fd >= MAX_FD {
            return;
        }
        let mut head = [0u8; 128];
        let mut close_now = false;
        let mut requeue = false;
        {
            let c = &mut self.clients[fd];
            if !c.live || c.generation != reply.generation {
                return;
            }
            c.outstanding = c.outstanding.saturating_sub(1);
            c.touched = now;
            let (a, b) = if reply.raw {
                (&reply.body[..], &[][..])
            } else {
                let n = build_head(reply.status, reply.body.len(), reply.close, &mut head);
                (&head[..n], &reply.body[..])
            };
            if c.output.is_empty() && c.sent == 0 {
                match send_iov(fd, a, b) {
                    Ok(sent) => {
                        let total = a.len() + b.len();
                        if sent < total {
                            append_unsent(c, a, b, sent);
                        } else if reply.close {
                            close_now = true;
                        }
                    }
                    Err(()) => close_now = true,
                }
            } else {
                c.output.extend_from_slice(a);
                c.output.extend_from_slice(b);
            }
            if !close_now {
                if reply.close {
                    if c.output.is_empty() && c.sent == 0 {
                        close_now = true;
                    } else {
                        c.close_when_flushed = true;
                    }
                }
                if c.output.len() > MAX_OUTPUT {
                    close_now = true;
                }
                if !close_now && c.paused && c.outstanding < OUTSTANDING_CAP {
                    c.paused = false;
                    if !c.input.is_empty() && !c.closing && !c.peer_closed {
                        c.ready = true;
                        requeue = true;
                    }
                }
            }
        }
        if close_now {
            self.close(fd);
            return;
        }
        if requeue {
            self.ready.push_back(fd);
        }
        let pending = {
            let c = &self.clients[fd];
            c.live && c.output.len() > c.sent
        };
        if pending {
            self.flush(fd, now);
            return;
        }
        let done = {
            let c = &self.clients[fd];
            c.live && c.peer_closed && c.outstanding == 0
        };
        if done {
            self.close(fd);
            return;
        }
        self.sync(fd);
    }

    fn drain(&mut self, replies: &Receiver<Reply>, now: u64) {
        loop {
            match replies.try_recv() {
                Ok(reply) => self.deliver(reply, now),
                Err(std::sync::mpsc::TryRecvError::Empty) => break,
                Err(std::sync::mpsc::TryRecvError::Disconnected) => break,
            }
        }
    }

    fn run(mut self, replies: Receiver<Reply>, wake: i32) -> io::Result<()> {
        let mut events = vec![libc::epoll_event { events: 0, u64: 0 }; 512];
        let mut scratch = vec![0u8; 65536];
        loop {
            let now = self.start.elapsed().as_secs();
            self.drain(&replies, now);
            self.process_ready();
            let timeout = if self.ready.is_empty() { 1000 } else { 0 };
            let n = unsafe {
                libc::epoll_wait(
                    self.ep,
                    events.as_mut_ptr(),
                    events.len() as i32,
                    timeout,
                )
            };
            if n < 0 {
                if io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                    continue;
                }
                return Err(io::Error::last_os_error());
            }
            for event in events.iter().take(n as usize) {
                let tag = event.u64;
                let flags = event.events;
                if tag == WAKE_TAG {
                    let mut buf = [0u8; 8];
                    unsafe {
                        libc::read(wake, buf.as_mut_ptr().cast(), buf.len());
                    }
                    self.drain(&replies, now);
                    continue;
                }
                if tag == self.listener as u64 {
                    self.accept(now);
                    continue;
                }
                let fd = tag as usize;
                if fd >= MAX_FD || !self.clients[fd].live {
                    continue;
                }
                if flags & libc::EPOLLERR as u32 != 0 {
                    self.close(fd);
                    continue;
                }
                if flags & libc::EPOLLHUP as u32 != 0
                    && flags & (libc::EPOLLIN | libc::EPOLLOUT) as u32 == 0
                {
                    self.close(fd);
                    continue;
                }
                if flags & libc::EPOLLOUT as u32 != 0 {
                    self.flush(fd, now);
                    if !self.clients[fd].live {
                        continue;
                    }
                }
                let can_read = {
                    let c = &self.clients[fd];
                    !c.paused && !c.closing && !c.peer_closed && c.output.len() == c.sent
                };
                if can_read && flags & (libc::EPOLLIN | libc::EPOLLRDHUP) as u32 != 0 {
                    self.handle_read(fd, &mut scratch, now);
                    if !self.clients[fd].live {
                        continue;
                    }
                }
                self.sync(fd);
            }
            if now >= self.last_sweep + 10 {
                self.last_sweep = now;
                for fd in 0..MAX_FD {
                    if !self.clients[fd].live {
                        continue;
                    }
                    let idle = {
                        let c = &self.clients[fd];
                        c.outstanding == 0 && now.saturating_sub(c.touched) >= 120
                    };
                    if idle {
                        self.close(fd);
                    }
                }
            }
        }
    }
}

pub fn run(
    reactor: u16,
    address: &str,
    secret: &str,
    jobs: Sender<Job>,
    replies: Receiver<Reply>,
    wake: i32,
) -> io::Result<()> {
    let reactor = Reactor::new(reactor, address, secret, jobs, wake)?;
    reactor.run(replies, wake)
}
