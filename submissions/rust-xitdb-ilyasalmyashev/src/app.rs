//! Routing and handlers. Each handler appends one complete HTTP response to `out`.
use crate::db::{Db, Kind, LikeState, Outcome, Pending, PostLookup};
use crate::http::Request;
use crate::json::{self, escape_into, push_int, Val};
use crate::jwt::{parse_positive, AuthError, Verifier};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

/// Who an answer goes to: (ring, slot, generation).
pub type Peer = (u8, u32, u16);

/// Answers that wait for a sync round, in one buffer.
#[derive(Default)]
pub struct Replies {
    bytes: Vec<u8>,
    /// Peer, end of its answer in `bytes`, and whether to close afterwards.
    ends: Vec<(Peer, u32, bool)>,
}

impl Replies {
    fn push(&mut self, peer: Peer, close: bool) {
        self.ends.push((peer, self.bytes.len() as u32, close));
    }
    /// Each answer, in order.
    pub fn iter(&self) -> impl Iterator<Item = (Peer, &[u8], bool)> {
        let mut start = 0;
        self.ends.iter().map(move |&(peer, end, close)| {
            let b = &self.bytes[start..end as usize];
            start = end as usize;
            (peer, b, close)
        })
    }
}

pub struct App {
    pub db: Db,
    jwt: Verifier,
    started: Instant,
    body: Vec<u8>,
    /// Validated writes of this pass, and the bytes of their bodies and usernames.
    pending: Vec<Pending>,
    arena: Vec<u8>,
    outcomes: Vec<Outcome>,
    /// Answers to the writes of the sync round that is running.
    flight: Replies,
    /// When the last sync round started.
    round_at: Instant,
}

const MAX_BODY_CHARS: usize = 500;
const ROUND_GAP: std::time::Duration = std::time::Duration::from_millis(5);

/// Writes the status line and headers, leaving room for a 4-digit Content-Length that is
/// fixed up by `finish`.
#[inline]
fn start(out: &mut Vec<u8>, status: u16) -> usize {
    out.extend_from_slice(match status {
        200 => b"HTTP/1.1 200 OK\r\n".as_slice(),
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    });
    out.extend_from_slice(b"Content-Type: application/json\r\nContent-Length: 0000\r\n\r\n");
    out.len()
}

/// Fills in Content-Length for the body that started at `body_start`.
#[inline]
fn finish(out: &mut Vec<u8>, body_start: usize) {
    let n = out.len() - body_start;
    let slot = body_start - 8; // "0000\r\n\r\n"
    let mut digits = [0u8; 20];
    let mut d = digits.len();
    let mut v = n;
    loop {
        d -= 1;
        digits[d] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    let digits = &digits[d..];
    if digits.len() != 4 {
        // Shift the body so the header has exactly the digits it needs.
        let delta = digits.len() as isize - 4;
        if delta > 0 {
            out.resize(out.len() + delta as usize, 0);
        }
        let new_start = (body_start as isize + delta) as usize;
        out.copy_within(body_start..body_start + n, new_start);
        out.truncate(new_start + n);
        out[slot..slot + digits.len()].copy_from_slice(digits);
        out[slot + digits.len()..new_start].copy_from_slice(b"\r\n\r\n");
    } else {
        out[slot..slot + 4].copy_from_slice(digits);
    }
}

pub fn error(out: &mut Vec<u8>, status: u16, msg: &str) {
    let b = start(out, status);
    out.extend_from_slice(b"{\"error\":\"");
    out.extend_from_slice(msg.as_bytes());
    out.extend_from_slice(b"\"}");
    finish(out, b);
}

fn post_id(s: &[u8]) -> Option<i64> {
    parse_positive(s)
}

fn now_secs() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

/// JavaScript's String.prototype.trim whitespace: Unicode White_Space without NEL, plus BOM.
fn is_js_space(c: char) -> bool {
    (c.is_whitespace() && c != '\u{85}') || c == '\u{feff}'
}

impl App {
    pub fn new(db: Db, secret: &[u8]) -> App {
        App {
            db,
            jwt: Verifier::new(secret),
            started: Instant::now(),
            body: Vec::with_capacity(2048),
            pending: Vec::new(),
            arena: Vec::new(),
            outcomes: Vec::new(),
            flight: Replies::default(),
            round_at: Instant::now(),
        }
    }

    #[cfg(feature = "stats")]
    pub fn has_pending(&self) -> bool {
        !self.pending.is_empty()
    }

    /// End of a pass: if no sync round is running, commits every write that has waited for one
    /// as a single transaction, and starts a round for it. While a round runs, writes just wait:
    /// their answers couldn't leave before the next round anyway. Rounds start at least
    /// ROUND_GAP apart, so that under load more writes share each fsync; a lone write on a quiet
    /// server starts one right away.
    pub fn commit_pass(&mut self) {
        if self.pending.is_empty() || self.db.syncing() || self.round_at.elapsed() < ROUND_GAP {
            return;
        }
        self.round_at = Instant::now();
        let ok = self.db.apply(&self.pending, &self.arena, &mut self.outcomes).is_ok();
        let r = &mut self.flight;
        if ok {
            for (p, o) in self.pending.iter().zip(&self.outcomes) {
                match (&p.kind, o) {
                    (Kind::Post { body, username, .. }, Outcome::Post { id, created }) => {
                        created_reply(&mut r.bytes, *id, &self.arena[range(*body)], created, &self.arena[range(*username)]);
                    }
                    (Kind::Like { post_id, .. }, Outcome::Like { new }) => like_reply(&mut r.bytes, *post_id, *new),
                    _ => unreachable!("one outcome per write"),
                }
                r.push(p.to, false);
            }
        } else {
            // None of them happened.
            for p in &self.pending {
                error(&mut r.bytes, 500, "internal server error");
                r.push(p.to, true);
            }
        }
        self.pending.clear();
        self.outcomes.clear();
        self.arena.clear();
        self.arena.shrink_to(1 << 20);
        if self.db.start_sync().is_err() {
            // The commit just written doesn't read back: nothing more can be promised.
            eprintln!("cannot read the last commit");
            std::process::exit(1);
        }
    }

    /// The answers whose writes just became durable, if any.
    #[inline]
    pub fn take_durable(&mut self) -> Option<Replies> {
        self.db.sync_finished().then(|| std::mem::take(&mut self.flight))
    }

    /// Gives back what `take_durable` returned, to reuse its memory.
    pub fn recycle(&mut self, mut r: Replies) {
        r.bytes.clear();
        r.ends.clear();
        r.bytes.shrink_to(1 << 20);
        r.ends.shrink_to(4096);
        self.flight = r;
    }

    /// Handles one request. The response is appended to `out`, or, when this returns true, the
    /// request is a write waiting for the end of the pass, and its answer comes from
    /// `take_durable`.
    pub fn handle(&mut self, req: &Request, out: &mut Vec<u8>, peer: Peer) -> bool {
        let p = req.path;
        match req.method {
            b"GET" => {
                if p == b"/feed" {
                    let mark = out.len();
                    let b = start(out, 200);
                    if self.db.feed(out).is_err() {
                        out.truncate(mark);
                        error(out, 500, "internal server error");
                        return false;
                    }
                    finish(out, b);
                } else if let Some(id) = p.strip_prefix(b"/posts/").filter(|id| !id.contains(&b'/')) {
                    let Some(id) = post_id(id) else {
                        error(out, 400, "invalid post id");
                        return false;
                    };
                    let mark = out.len();
                    let b = start(out, 200);
                    match self.db.post(id, out) {
                        Ok(PostLookup::Found) => finish(out, b),
                        Ok(PostLookup::Missing) => {
                            out.truncate(mark);
                            error(out, 404, "post not found")
                        }
                        Err(_) => {
                            out.truncate(mark);
                            error(out, 500, "internal server error")
                        }
                    }
                } else if p == b"/health" {
                    self.health(out)
                } else {
                    error(out, 404, "not found")
                }
                false
            }
            b"POST" => {
                if p == b"/posts" {
                    self.create(req, out, peer)
                } else if let Some(rest) = p.strip_prefix(b"/posts/").and_then(|r| r.strip_suffix(b"/like")).filter(|id| !id.contains(&b'/')) {
                    self.like(req, rest, out, peer)
                } else {
                    error(out, 404, "not found");
                    false
                }
            }
            _ => {
                error(out, 404, "not found");
                false
            }
        }
    }

    fn health(&mut self, out: &mut Vec<u8>) {
        match self.db.health() {
            Ok(()) => {
                let b = start(out, 200);
                out.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                push_int(out, self.started.elapsed().as_secs() as i64);
                out.push(b'}');
                finish(out, b);
            }
            Err(e) => {
                let b = start(out, 503);
                out.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                escape_into(out, e.as_bytes());
                out.extend_from_slice(b"\"}");
                finish(out, b);
            }
        }
    }

    /// Ok(user id, username) or an error response already written.
    fn auth(&mut self, req: &Request, out: &mut Vec<u8>) -> Option<(i64, Vec<u8>)> {
        match self.jwt.verify(req.auth, now_secs()) {
            Ok(c) => Some((c.user_id, c.username)),
            Err(AuthError::Missing) => {
                error(out, 401, "missing bearer token");
                None
            }
            Err(AuthError::Invalid) => {
                error(out, 401, "invalid or expired token");
                None
            }
            Err(AuthError::Payload) => {
                error(out, 401, "invalid token payload");
                None
            }
        }
    }

    /// Validates a new post and queues it. True if it's queued.
    fn create(&mut self, req: &Request, out: &mut Vec<u8>, peer: Peer) -> bool {
        let Some((user_id, username)) = self.auth(req, out) else { return false };
        let mut body: Option<(&[u8], bool)> = None;
        let parsed = json::parse_object(req.body, &mut |k, v| {
            if k == b"body" {
                body = match v {
                    Val::Str(s, e) => Some((s, e)),
                    _ => None,
                };
            }
        });
        match parsed {
            Err(()) => {
                error(out, 400, "malformed JSON body");
                return false;
            }
            Ok(false) => {
                error(out, 400, "body is required");
                return false;
            }
            Ok(true) => {}
        }
        let Some((raw, esc)) = body else {
            error(out, 400, "body is required");
            return false;
        };
        let text: &[u8] = if esc {
            self.body.clear();
            json::unescape(raw, &mut self.body);
            &self.body
        } else {
            raw
        };
        // Validated as UTF-8 by the JSON reader (and unescape writes UTF-8).
        let s = std::str::from_utf8(text).unwrap_or("");
        let s = s.trim_matches(is_js_space);
        if s.is_empty() {
            error(out, 400, "body is required");
            return false;
        }
        if s.len() > MAX_BODY_CHARS && s.chars().count() > MAX_BODY_CHARS {
            error(out, 400, "body must be at most 500 characters");
            return false;
        }
        let a = self.arena.len() as u32;
        self.arena.extend_from_slice(s.as_bytes());
        let b = self.arena.len() as u32;
        self.arena.extend_from_slice(&username);
        let c = self.arena.len() as u32;
        self.pending.push(Pending { to: peer, kind: Kind::Post { user_id, body: (a, b), username: (b, c) } });
        true
    }

    /// Answers a like that needs no write, or queues it. True if it's queued.
    fn like(&mut self, req: &Request, id: &[u8], out: &mut Vec<u8>, peer: Peer) -> bool {
        let Some((user_id, _)) = self.auth(req, out) else { return false };
        let Some(post_id) = post_id(id) else {
            error(out, 400, "invalid post id");
            return false;
        };
        match self.db.like_state(user_id, post_id) {
            Ok(LikeState::New) => {
                self.pending.push(Pending { to: peer, kind: Kind::Like { user_id, post_id } });
                true
            }
            Ok(LikeState::Already) => {
                like_reply(out, post_id, false);
                false
            }
            Ok(LikeState::Missing) => {
                error(out, 404, "post not found");
                false
            }
            Err(_) => {
                error(out, 500, "internal server error");
                false
            }
        }
    }
}

fn range(r: (u32, u32)) -> std::ops::Range<usize> {
    r.0 as usize..r.1 as usize
}

fn created_reply(out: &mut Vec<u8>, id: i64, body: &[u8], created: &[u8], username: &[u8]) {
    let b = start(out, 201);
    out.extend_from_slice(b"{\"post\":{\"id\":");
    push_int(out, id);
    out.extend_from_slice(b",\"body\":\"");
    escape_into(out, body);
    out.extend_from_slice(b"\",\"created_at\":\"");
    escape_into(out, created);
    out.extend_from_slice(b"\",\"author\":\"");
    escape_into(out, username);
    out.extend_from_slice(b"\",\"like_count\":0}}");
    finish(out, b);
}

fn like_reply(out: &mut Vec<u8>, id: i64, new: bool) {
    let b = start(out, if new { 201 } else { 200 });
    out.extend_from_slice(if new {
        b"{\"liked\":true,\"already_liked\":false,\"post_id\":".as_slice()
    } else {
        b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
    });
    push_int(out, id);
    out.push(b'}');
    finish(out, b);
}
