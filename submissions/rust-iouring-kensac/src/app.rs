//! Routing and handlers. Each handler appends one complete HTTP response to `out`.
use crate::db::{Db, PostLookup};
use crate::http::Request;
use crate::json::{self, escape_into, push_int, Val};
use crate::jwt::{parse_positive, AuthError, Verifier};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

pub struct App {
    pub db: Db,
    jwt: Verifier,
    started: Instant,
    body: Vec<u8>,
    /// Set when the batch wrote: its responses must wait for COMMIT (they always do).
    pub wrote: bool,
}

const MAX_BODY_CHARS: usize = 500;

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
        App { db, jwt: Verifier::new(secret), started: Instant::now(), body: Vec::with_capacity(2048), wrote: false }
    }

    #[cfg(feature = "bench")]
    pub fn bench_verify(&mut self, h: &[u8]) -> bool {
        self.jwt.verify(Some(h), now_secs()).is_ok()
    }

    /// Handles one request; the response is appended to `out`.
    pub fn handle(&mut self, req: &Request, out: &mut Vec<u8>) {
        let p = req.path;
        match req.method {
            b"GET" => {
                if p == b"/feed" {
                    let mark = out.len();
                    let b = start(out, 200);
                    if self.db.feed(out).is_err() {
                        out.truncate(mark);
                        return error(out, 500, "internal server error");
                    }
                    finish(out, b);
                } else if let Some(id) = p.strip_prefix(b"/posts/").filter(|id| !id.contains(&b'/')) {
                    let Some(id) = post_id(id) else {
                        return error(out, 400, "invalid post id");
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
            }
            b"POST" => {
                if p == b"/posts" {
                    self.create(req, out)
                } else if let Some(rest) = p.strip_prefix(b"/posts/").and_then(|r| r.strip_suffix(b"/like")).filter(|id| !id.contains(&b'/')) {
                    self.like(req, rest, out)
                } else {
                    error(out, 404, "not found")
                }
            }
            _ => error(out, 404, "not found"),
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

    fn create(&mut self, req: &Request, out: &mut Vec<u8>) {
        let Some((uid, username)) = self.auth(req, out) else { return };
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
            Err(()) => return error(out, 400, "malformed JSON body"),
            Ok(false) => return error(out, 400, "body is required"),
            Ok(true) => {}
        }
        let Some((raw, esc)) = body else {
            return error(out, 400, "body is required");
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
            return error(out, 400, "body is required");
        }
        if s.len() > MAX_BODY_CHARS && s.chars().count() > MAX_BODY_CHARS {
            return error(out, 400, "body must be at most 500 characters");
        }
        let s = s.as_bytes();
        match self.db.insert_post(uid, s) {
            Ok((id, created, n)) => {
                self.wrote = true;
                let b = start(out, 201);
                out.extend_from_slice(b"{\"post\":{\"id\":");
                push_int(out, id);
                out.extend_from_slice(b",\"body\":\"");
                escape_into(out, s);
                out.extend_from_slice(b"\",\"created_at\":\"");
                escape_into(out, &created[..n]);
                out.extend_from_slice(b"\",\"author\":\"");
                escape_into(out, &username);
                out.extend_from_slice(b"\",\"like_count\":0}}");
                finish(out, b);
            }
            Err(_) => error(out, 500, "internal server error"),
        }
    }

    fn like(&mut self, req: &Request, id: &[u8], out: &mut Vec<u8>) {
        let Some((uid, _)) = self.auth(req, out) else { return };
        let Some(id) = post_id(id) else {
            return error(out, 400, "invalid post id");
        };
        match self.db.post_exists(id) {
            Ok(true) => {}
            Ok(false) => return error(out, 404, "post not found"),
            Err(_) => return error(out, 500, "internal server error"),
        }
        match self.db.insert_like(uid, id) {
            Ok(new) => {
                self.wrote |= new;
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
            Err(_) => error(out, 500, "internal server error"),
        }
    }
}
