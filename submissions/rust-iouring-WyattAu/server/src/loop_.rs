//! The event loop (ADR 0003): one thread, uring-kit transport, pass-based
//! group commit. Reads run at one snapshot; writes batch through rusqlite;
//! COMMIT lands before the pass's responses flush; reader.refresh() at the
//! start of the next pass advances the snapshot.
//!
//! Transport (exact uring-kit semantics):
//! - `add_listener` arms a single-shot Accept SQE; the engine re-arms it on
//!   every completion (drain path). The Accept CQE's result IS the new fd.
//! - reads: registered-buffer `read(token, fd, slot)`; CQE result = bytes;
//!   0 = peer close.
//! - sends: `write_vectored` from per-connection outbox buffers (stable
//!   until the write CQE; popped after). One write in flight per connection;
//!   on its completion: pop outbox front, send next, else re-arm read.
//!
//! Pass order: drain CQEs -> parse -> reads at the current snapshot ->
//! write batch (BEGIN; inserts; COMMIT) -> responses built from post-commit
//! data -> send -> (next pass) reader.refresh() advances the snapshot.

use std::collections::{HashMap, VecDeque};
use std::io;
use std::net::SocketAddr;
use std::os::fd::{AsRawFd, RawFd};
use std::time::Duration;

use uring_http::http::{parse, ParseError};
use uring_http::json;
use uring_kit::engine::{Engine, Cqe};
use uring_kit::net;
use uring_kit::token::{Op, Token};

use crate::config::Config;
use crate::handlers;
use crate::router::{self, errors, Dispatch, Route};
use crate::store::Store;

const RECV_BUF_SIZE: usize = 2048;
const RING_ENTRIES: u32 = 4096;
const POOL_CAPACITY: usize = 16_384; // max concurrent connections (F55 budget)
/// hard cap on request head+body bytes (SPEC bodies <= 500 chars)
const MAX_REQUEST: usize = 64 * 1024;
/// pipelined requests drained per read burst
const MAX_PIPELINED: usize = 8;

struct Conn {
    fd: RawFd,
    generation: u16,
    /// fixed per-connection receive buffer (unregistered; stable address)
    recv_buf: Vec<u8>,
    /// request-stream accumulation
    buf: Vec<u8>,
    /// consumed prefix of `buf`
    consumed: usize,
    /// responses awaiting send (front is in flight)
    outbox: VecDeque<Vec<u8>>,
    last_active: u64,
}

struct Queued {
    fd: RawFd,
    generation: u16,
    kind: QueuedKind,
}

#[derive(Clone)]
enum QueuedKind {
    Health,
    Feed,
    PostGet(i64),
    PostCreate { user: i64, username: Vec<u8>, body: Vec<u8> },
    PostLike { user: i64, post: i64 },
    Error((u16, &'static str)),
}

pub struct Server {
    cfg: Config,
    store: Store,
    engine: uring_kit::uring::UringEngine,
    #[allow(dead_code)]
    listener: std::net::TcpListener,
    accept_fd: RawFd,
    conns: HashMap<RawFd, Conn>,
    /// generation -> fd (one live conn per generation)
    gen_conn: HashMap<u16, RawFd>,
    /// closed conns awaiting their close(2) CQE; buffers stay alive so an
    /// in-flight kernel write/read can never touch freed memory
    graveyard: Vec<(Token, Conn)>,
    queue: Vec<Queued>,
    now_ms: u64,
}

impl Server {
    pub fn new(cfg: Config) -> Result<Self, String> {
        let store = Store::open(&cfg.sqlite_path, &cfg.jwt_secret)?;
        let engine =
            uring_kit::uring::UringEngine::new(RING_ENTRIES, None, false)
                .map_err(|e| format!("ring setup: {e}"))?;
        let addr: SocketAddr = format!("{}:{}", cfg.host, cfg.port)
            .parse()
            .map_err(|e| format!("bind addr: {e}"))?;
        let listener =
            net::tcp_listener(addr, false, 65_535).map_err(|e| format!("listen: {e}"))?;
        let accept_fd = listener.as_raw_fd();
        Ok(Self {
            cfg,
            store,
            engine,
            listener,
            accept_fd,
            conns: HashMap::new(),
            gen_conn: HashMap::new(),
            graveyard: Vec::new(),
            queue: Vec::new(),
            now_ms: 0,
        })
    }

    pub fn run(&mut self) -> io::Result<()> {
        let accept_token = Token::accept(0);
        self.engine.add_listener(self.accept_fd, accept_token)?;
        let mut cqes: Vec<Cqe> = Vec::with_capacity(256);
        let dbg = std::env::var_os("LOOP_DEBUG").is_some();
        loop {
            self.now_ms = now_ms();
            cqes.clear();
            if dbg {
                eprintln!("loop: calling poll");
            }
            self.engine.poll(Some(Duration::from_millis(250)), &mut cqes)?;
            if dbg {
                eprintln!("poll returned: {} cqes", cqes.len());
            }
            for cqe in &cqes {
                self.on_cqe(cqe);
            }
            if !self.queue.is_empty() {
                self.work_pass();
            }
            self.idle_close();
        }
    }

    fn on_cqe(&mut self, cqe: &Cqe) {
        match cqe.token.op() {
            Op::Accept => {
                if let Ok(fd32) = cqe.result {
                    self.admit(fd32 as RawFd);
                }
                // the engine re-arms the listener itself (drain path)
            }
            Op::Read => {
                let generation = cqe.token.generation();
                let n = match cqe.result {
                    Ok(n) => n as usize,
                    Err(_) => {
                        self.close_conn(generation);
                        return;
                    }
                };
                let Some(&fd) = self.gen_conn.get(&generation) else {
                    return;
                };
                if n == 0 {
                    // peer closed
                    self.close_conn(generation);
                    return;
                }
                let bytes: &[u8] = {
                    let conn = match self.conns.get(&fd) {
                        Some(c) if c.generation == generation => c,
                        _ => return,
                    };
                    &conn.recv_buf[..n.min(conn.recv_buf.len())]
                };
                let bytes = bytes.to_vec();
                let conn = match self.conns.get_mut(&fd) {
                    Some(c) if c.generation == generation => c,
                    _ => return,
                };
                conn.last_active = self.now_ms;
                conn.buf.extend_from_slice(&bytes);
                self.consume(fd, generation);
            }
            Op::Write => {
                let generation = cqe.token.generation();
                if cqe.result.is_err() {
                    self.close_conn(generation);
                    return;
                }
                // response sent: pop the outbox front; send next or re-arm read
                let Some(&fd) = self.gen_conn.get(&generation) else { return };
                let Some(conn) = self.conns.get_mut(&fd) else { return };
                if conn.generation != generation {
                    return;
                }
                conn.outbox.pop_front();
                if conn.outbox.front().is_some() {
                    self.submit_write_front(fd, generation);
                } else {
                    self.submit_read(fd, generation);
                }
            }
            Op::Close => {
                // close(2) landed: free the graveyard entry now
                if let Some(pos) = self
                    .graveyard
                    .iter()
                    .position(|(t, _)| *t == cqe.token)
                {
                    self.graveyard.swap_remove(pos);
                }
            }
            Op::Connect | Op::Splice => {}
        }
    }

    /// Parse accumulated bytes; queue every complete request. Unconsumed
    /// bytes stay in `conn.buf` (compacted periodically).
    fn consume(&mut self, fd: RawFd, generation: u16) {
        for _ in 0..MAX_PIPELINED {
            let (buf, consumed) = {
                let Some(conn) = self.conns.get(&fd) else { return };
                (conn.buf.clone(), conn.consumed)
            };
            let slice = &buf[consumed..];
            if slice.is_empty() {
                if let Some(conn) = self.conns.get_mut(&fd) {
                    conn.buf.clear();
                    conn.consumed = 0;
                }
                return;
            }
            match parse(slice) {
                Ok(req) => {
                    if std::env::var_os("LOOP_DEBUG").is_some() {
                        eprintln!(
                            "parsed fd={fd} consumed={consumed} slice_len={} total={} method={:?} target={:?} cl={:?} head={:?}",
                            slice.len(),
                            head_len(slice).saturating_add(req.content_length.unwrap_or(0)),
                            req.method,
                            req.target,
                            req.content_length,
                            &slice[..slice.len().min(60)]
                        );
                    }
                    let head_end = head_len(slice);
                    let content_length = req.content_length.unwrap_or(0);
                    let total = head_end.saturating_add(content_length);
                    if total > MAX_REQUEST {
                        self.queue_error(fd, generation, errors::NOT_FOUND);
                        self.close_conn(generation);
                        return;
                    }
                    if slice.len() < total {
                        // body incomplete: wait for more bytes
                        self.submit_read(fd, generation);
                        return;
                    }
                    let body = &slice[head_end..total];
                    self.enqueue(fd, generation, &req, body);
                    if let Some(conn) = self.conns.get_mut(&fd) {
                        conn.consumed = total;
                        if conn.consumed > 32 * 1024 {
                            conn.buf.drain(..conn.consumed);
                            conn.consumed = 0;
                        }
                    }
                }
                Err(ParseError::Incomplete) => {
                    if slice.len() > MAX_REQUEST {
                        self.close_conn(generation);
                        return;
                    }
                    self.submit_read(fd, generation);
                    return;
                }
                Err(_) => {
                    // malformed head: not a SPEC route
                    self.queue_error(fd, generation, errors::NOT_FOUND);
                    self.close_conn(generation);
                    return;
                }
            }
        }
    }

    fn enqueue(
        &mut self,
        fd: RawFd,
        generation: u16,
        req: &uring_http::http::Request<'_>,
        body: &[u8],
    ) {
        let now = (self.now_ms / 1000) as i64;
        let mac = &self.store.keyed_mac;
        let kind = match router::dispatch(req) {
            Dispatch::Route(Route::Health, _) => QueuedKind::Health,
            Dispatch::Route(Route::Feed, _) => QueuedKind::Feed,
            Dispatch::Route(Route::PostGet, _) => {
                let id = parse_positive(&req.target[7..]).unwrap_or(0);
                QueuedKind::PostGet(id)
            }
            Dispatch::Route(Route::PostCreate, _) => {
                // auth FIRST (SPEC precedence)
                let claims = match crate::jwt::verify(req.authorization, mac, now) {
                    Ok(c) => c,
                    Err(e) => {
                        self.queue_error(fd, generation, router::jwt_row(e));
                        return;
                    }
                };
                match body_field(body) {
                    Err(_) => QueuedKind::Error(errors::MALFORMED_JSON),
                    Ok(BodyExtract::Missing) => QueuedKind::Error(errors::BODY_REQUIRED),
                    Ok(BodyExtract::TooLong) => QueuedKind::Error(errors::BODY_TOO_LONG),
                    Ok(BodyExtract::Body(b)) if b.is_empty() => {
                        QueuedKind::Error(errors::BODY_REQUIRED)
                    }
                    Ok(BodyExtract::Body(b)) => QueuedKind::PostCreate {
                        user: claims.user_id,
                        username: claims.username,
                        body: b,
                    },
                }
            }
            Dispatch::Route(Route::PostLike, _) => {
                // auth BEFORE id parse (SPEC)
                let claims = match crate::jwt::verify(req.authorization, mac, now) {
                    Ok(c) => c,
                    Err(e) => {
                        self.queue_error(fd, generation, router::jwt_row(e));
                        return;
                    }
                };
                // id from the target: /posts/<id>/like
                let t = &req.target[7..];
                let id_seg = &t[..t.len().saturating_sub(5)];
                let post = match parse_positive(id_seg) {
                    Some(v) => v,
                    None => {
                        self.queue_error(fd, generation, errors::INVALID_POST_ID);
                        return;
                    }
                };
                QueuedKind::PostLike { user: claims.user_id, post }
            }
            Dispatch::Error(err) => QueuedKind::Error(err),
        };
        self.queue.push(Queued { fd, generation, kind });
    }

    fn queue_error(&mut self, fd: RawFd, generation: u16, err: (u16, &'static str)) {
        self.queue.push(Queued { fd, generation, kind: QueuedKind::Error(err) });
    }

    fn submit_read(&mut self, fd: RawFd, generation: u16) {
        // unregistered vectored read into the connection's own stable buffer
        let iov = Box::new([libc::iovec {
            iov_base: self.recv_ptr(fd),
            iov_len: RECV_BUF_SIZE,
        }]);
        let token = Token::new(Op::Read, 0, generation, 0);
        if self.engine.read_vectored(token, fd, iov).is_err() {
            self.close_conn(generation);
        }
    }

    /// Raw recv-buffer pointer for the in-flight read. SAFETY (uring-kit
    /// contract): the buffer is a fixed-capacity Vec that is never resized,
    /// its address is stable for the connection's lifetime, and the kernel
    /// writes only between the read submission and its CQE on this thread.
    fn recv_ptr(&self, fd: RawFd) -> *mut libc::c_void {
        self.conns
            .get(&fd)
            .map(|c| c.recv_buf.as_ptr() as *mut libc::c_void)
            .unwrap_or(std::ptr::null_mut())
    }

    /// Submit the outbox front. SAFETY (uring-kit contract): the frame is
    /// owned by the outbox and stays at a stable address until its write
    /// CQE pops it — the iov must not outlive that.
    fn submit_write_front(&mut self, fd: RawFd, generation: u16) {
        let Some(conn) = self.conns.get(&fd) else { return };
        let Some(front) = conn.outbox.front() else { return };
        let iov = Box::new([libc::iovec {
            iov_base: front.as_ptr() as *mut libc::c_void,
            iov_len: front.len(),
        }]);
        let _ = conn; // borrow ends here; iov points at outbox-owned memory
        let token = Token::new(Op::Write, 0, generation, 0);
        if self.engine.write_vectored(token, fd, iov).is_err() {
            self.close_conn(generation);
        }
    }

    fn admit(&mut self, fd: RawFd) {
        if self.conns.len() >= POOL_CAPACITY {
            // connection budget exhausted (F55): close; k6 reconnects
            unsafe { libc::close(fd) };
            return;
        }
        self.engine
            .add_stream(fd, Token::new(Op::Read, 0, 0, 0));
        let generation = 0;
        self.conns.insert(
            fd,
            Conn {
                fd,
                generation,
                recv_buf: vec![0u8; RECV_BUF_SIZE],
                buf: Vec::new(),
                consumed: 0,
                outbox: VecDeque::new(),
                last_active: self.now_ms,
            },
        );
        self.gen_conn.insert(generation, fd);
        self.submit_read(fd, generation);
    }

    fn close_conn(&mut self, generation: u16) {
        let Some(&fd) = self.gen_conn.get(&generation) else { return };
        let Some(conn) = self.conns.remove(&fd) else { return };
        self.gen_conn.remove(&generation);
        // DEFERRED FREE: the conn (and its recv_buf) stays alive in the
        // graveyard until the close CQE lands; any in-flight kernel op on
        // this fd then completes into memory we still own, or fails EBADF
        // if close(2) executed first. Either way: no use-after-free.
        let token = Token::new(Op::Close, 0, generation, 0);
        self.graveyard.push((token, conn));
        let _ = self.engine.close(token, fd);
    }

    /// One pass. Reads run at the current snapshot; writes batch in one
    /// transaction; COMMIT precedes response flushes; the reader snapshot
    /// advances at the NEXT pass's start (ADR 0003).
    fn work_pass(&mut self) {
        let queue = std::mem::take(&mut self.queue);
        // (status, body) per queued request, filled in three phases
        let mut results: Vec<(u16, Vec<u8>)> = vec![(200, Vec::new()); queue.len()];

        // phase 1: reads (pre-write snapshot)
        for (i, q) in queue.iter().enumerate() {
            let (status, body) = match &q.kind {
                QueuedKind::Health => {
                    if self.store.health_db() {
                        (
                            200u16,
                            format!(
                                "{{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":{}}}",
                                self.store.uptime_s()
                            )
                            .into_bytes(),
                        )
                    } else {
                        (
                            503u16,
                            b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"select 1 failed\"}"
                                .to_vec(),
                        )
                    }
                }
                QueuedKind::Feed => {
                    // E18 zero-copy emission: rows streamed from the mmap
                    // during the backward walk (no PostFull intermediates)
                    let mut body = Vec::with_capacity(6144);
                    match handlers::reader_feed_emit(&self.store, 20, &mut body) {
                        Ok(()) => (200u16, body),
                        Err(_) => {
                            // degradation ladder: fallback feed (SPEC SQL)
                            let mut b = Vec::with_capacity(6144);
                            b.extend_from_slice(b"{\"posts\":[");
                            let feed =
                                handlers::fallback_feed(&self.store.writer_conn(), 20);
                            for (i, p) in feed.iter().enumerate() {
                                if i > 0 {
                                    b.push(b',');
                                }
                                let pref = json::PostRef {
                                    id: p.id,
                                    body: &p.body,
                                    created_at: &p.created_at,
                                    author: &p.author,
                                    like_count: p.like_count,
                                };
                                let mut tmp = [0u8; 2048];
                                let n = json::write_post_json(&mut tmp, &pref);
                                b.extend_from_slice(&tmp[..n]);
                            }
                            b.extend_from_slice(b"]}");
                            (200u16, b)
                        }
                    }
                }
                QueuedKind::PostGet(id) => match handlers::read_post(&self.store, &self.store.writer_conn(), *id) {
                    Some(row) => {
                        let author = handlers::read_username(&self.store, &self.store.writer_conn(), row.user_id)
                            .unwrap_or_default();
                        let lc = handlers::read_like_count(&self.store, &self.store.writer_conn(), row.id).unwrap_or(0);
                        let pref = json::PostRef {
                            id: row.id,
                            body: &row.body,
                            created_at: &row.created_at,
                            author: &author,
                            like_count: lc,
                        };
                        let mut tmp = [0u8; 2048];
                        let n = json::write_post_json(&mut tmp, &pref);
                        // SPEC: GET /posts/:id responds {"post":{...}}
                        let mut wrapped = Vec::with_capacity(n + 10);
                        wrapped.extend_from_slice(b"{\"post\":");
                        wrapped.extend_from_slice(&tmp[..n]);
                        wrapped.push(b'}');
                        (200u16, wrapped)
                    }
                    None => (404u16, error_body(errors::POST_NOT_FOUND).into_bytes()),
                },
                _ => (200u16, Vec::new()), // writes + errors below
            };
            results[i] = (status, body);
        }

        // phase 2: writes batched in one transaction (COMMIT before flushes)
        let _ = self.store.writer.execute_batch("BEGIN DEFERRED");
        for (i, q) in queue.iter().enumerate() {
            match &q.kind {
                QueuedKind::PostCreate { user, username, body } => {
                    match handlers::insert_post(&self.store.writer, *user, body) {
                        Ok((pid, created)) => {
                            let pref = json::PostRef {
                                id: pid,
                                body,
                                created_at: &created,
                                author: username,
                                like_count: 0,
                            };
                            let mut tmp = [0u8; 2048];
                            let n = json::write_post_json(&mut tmp, &pref);
                            // SPEC: POST /posts responds {"post":{...}}
                            let mut wrapped = Vec::with_capacity(n + 10);
                            wrapped.extend_from_slice(b"{\"post\":");
                            wrapped.extend_from_slice(&tmp[..n]);
                            wrapped.push(b'}');
                            results[i] = (201, wrapped);
                        }
                        Err(_) => results[i] = (500, error_body(errors::INTERNAL).into_bytes()),
                    }
                }
                QueuedKind::PostLike { user, post } => {
                    // existence inside the txn: 404 precedence
                    let exists = self
                        .store
                        .writer
                        .query_row("SELECT 1 FROM posts WHERE id = ?1", [post], |_| Ok(()))
                        .is_ok();
                    if !exists {
                        results[i] = (404, error_body(errors::POST_NOT_FOUND).into_bytes());
                        continue;
                    }
                    results[i] = match handlers::insert_like(&self.store.writer, *user, *post) {
                        Ok(true) => (
                            201,
                            format!(
                                "{{\"liked\":true,\"already_liked\":false,\"post_id\":{}}}",
                                post
                            )
                            .into_bytes(),
                        ),
                        Ok(false) => (
                            200,
                            format!(
                                "{{\"liked\":true,\"already_liked\":true,\"post_id\":{}}}",
                                post
                            )
                            .into_bytes(),
                        ),
                        Err(_) => (500, error_body(errors::INTERNAL).into_bytes()),
                    };
                }
                _ => {}
            }
        }
        let commit_ok = self.store.writer.execute_batch("COMMIT").is_ok();
        if !commit_ok {
            for (i, q) in queue.iter().enumerate() {
                if matches!(q.kind, QueuedKind::PostCreate { .. } | QueuedKind::PostLike { .. }) {
                    results[i] = (500, error_body(errors::INTERNAL).into_bytes());
                }
            }
        }

        // phase 3: errors keep their SPEC rows even if a read ran first
        for (i, q) in queue.iter().enumerate() {
            if let QueuedKind::Error(err) = &q.kind {
                results[i] = (err.0, error_body(*err).into_bytes());
            }
        }

        // phase 4: COMMIT landed -> flush responses
        for ((i, q), (status, body)) in queue.iter().enumerate().zip(results.iter()) {
            if std::env::var_os("LOOP_DEBUG").is_some() {
                eprintln!("send fd={} status={} len={}", q.fd, status, body.len());
            }
            self.send_response(q.fd, q.generation, *status, body);
        }

        // snapshot advances AFTER commit for the next pass (ADR 0003)
        self.store.refresh_snapshot();
    }

    fn send_response(
        &mut self,
        fd: RawFd,
        generation: u16,
        status: u16,
        body: &[u8],
    ) {
        if std::env::var_os("LOOP_DEBUG").is_some() {
            eprintln!("send_response fd {fd} status {status} len {}", body.len());
        }
        let head = format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: keep-alive\r\n\r\n",
            status,
            reason_phrase(status),
            body.len()
        );
        let mut frame = head.into_bytes();
        frame.extend_from_slice(body);
        let was_idle = match self.conns.get_mut(&fd) {
            Some(conn) => {
                let was_idle = conn.outbox.is_empty();
                conn.outbox.push_back(frame);
                was_idle
            }
            None => {
                self.close_conn(generation);
                return;
            }
        };
        if was_idle {
            self.submit_write_front(fd, generation);
        }
    }

    fn idle_close(&mut self) {
        const IDLE_MS: u64 = 65_000;
        let stale: Vec<RawFd> = self
            .conns
            .iter()
            .filter(|(_, c)| self.now_ms.saturating_sub(c.last_active) > IDLE_MS)
            .map(|(fd, _)| *fd)
            .collect();
        for fd in stale {
            if let Some(c) = self.conns.remove(&fd) {
                self.gen_conn.remove(&c.generation);
                // DEFERRED FREE: keep the conn alive until the close CQE
                let token = Token::new(Op::Close, 0, c.generation, 0);
                self.graveyard.push((token, c));
                let _ = self.engine.close(token, fd);
            }
        }
    }
}

fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "OK",
    }
}

fn error_body(err: (u16, &'static str)) -> String {
    format!("{{\"error\":\"{}\"}}", err.1)
}

fn parse_positive(seg: &[u8]) -> Option<i64> {
    if seg.is_empty() || seg.len() > 18 {
        return None;
    }
    let mut v: i64 = 0;
    for &b in seg {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    (v > 0).then_some(v)
}

fn head_len(slice: &[u8]) -> usize {
    memchr_find(slice, b"\r\n\r\n").map_or(0, |p| p + 4)
}

fn memchr_find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.len() > haystack.len() {
        return None;
    }
    (0..=haystack.len() - needle.len()).find(|&i| &haystack[i..i + needle.len()] == needle)
}

/// Body extraction outcome for POST /posts (SPEC error rows map 1:1).
enum BodyExtract {
    /// no "body" key, or empty after trim -> `body is required`
    Missing,
    /// trimmed length > 500 -> `body must be at most 500 characters`
    TooLong,
    /// the trimmed body bytes
    Body(Vec<u8>),
}

/// Extract the `body` string field from a flat JSON object, JSON-unescape
/// the minimal escape set, and trim ASCII whitespace.
fn body_field(raw_body: &[u8]) -> Result<BodyExtract, ()> {
    // a body that is not a JSON object at all is MALFORMED; a JSON object
    // without a usable "body" string is MISSING (SPEC error rows)
    let t = trim_ascii(raw_body);
    if t.first() != Some(&b'{') || t.last() != Some(&b'}') {
        return Err(());
    }
    let key = b"\"body\"";
    let mut i = 0;
    while i + key.len() <= raw_body.len() {
        if &raw_body[i..i + key.len()] == key {
            let mut j = i + key.len();
            while j < raw_body.len() && (raw_body[j] == b' ' || raw_body[j] == b':') {
                j += 1;
            }
            if j < raw_body.len() && raw_body[j] == b'"' {
                let mut out = Vec::new();
                let mut k = j + 1;
                while k < raw_body.len() {
                    let b = raw_body[k];
                    match b {
                        b'"' => {
                            let t = trim_ascii(&out);
                            if t.len() > 500 {
                                return Ok(BodyExtract::TooLong);
                            }
                            if t.is_empty() {
                                return Ok(BodyExtract::Missing);
                            }
                            return Ok(BodyExtract::Body(t.to_vec()));
                        }
                        b'\\' => {
                            k += 1;
                            if k >= raw_body.len() {
                                return Err(());
                            }
                            match raw_body[k] {
                                b'"' => out.push(b'"'),
                                b'\\' => out.push(b'\\'),
                                b'/' => out.push(b'/'),
                                b'n' => out.push(b'\n'),
                                b't' => out.push(b'\t'),
                                b'r' => out.push(b'\r'),
                                b'b' => out.push(0x08),
                                b'f' => out.push(0x0c),
                                b'u' => {
                                    if k + 4 >= raw_body.len() {
                                        return Err(());
                                    }
                                    let hex = std::str::from_utf8(&raw_body[k + 1..k + 5])
                                        .map_err(|_| ())?;
                                    let cp = u32::from_str_radix(hex, 16).map_err(|_| ())?;
                                    let ch = char::from_u32(cp).ok_or(())?;
                                    let mut enc = [0u8; 4];
                                    out.extend_from_slice(ch.encode_utf8(&mut enc).as_bytes());
                                    k += 4;
                                }
                                _ => return Err(()),
                            }
                            k += 1;
                        }
                        other => {
                            out.push(other);
                            k += 1;
                        }
                    }
                }
                return Err(()); // unterminated string
            }
        }
        i += 1;
    }
    // well-formed JSON object without a "body" member
    Ok(BodyExtract::Missing)
}

fn trim_ascii(b: &[u8]) -> &[u8] {
    let mut start = 0;
    let mut end = b.len();
    while start < end && matches!(b[start], b' ' | b'\t' | b'\n' | b'\r') {
        start += 1;
    }
    while end > start && matches!(b[end - 1], b' ' | b'\t' | b'\n' | b'\r') {
        end -= 1;
    }
    &b[start..end]
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
