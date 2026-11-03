//! The API: routes, validation, JWT verification and response framing.
//! Transport-independent, so the epoll and io_uring loops share it.

use std::time::{SystemTime, UNIX_EPOCH};

use crate::crypto::{ct_eq32, HmacKey};
use crate::db::{Db, LikeResult};
use crate::http::{self, Request};
use crate::json;

pub const R_NOT_FOUND: &[u8] = b"{\"error\":\"not found\"}";
pub const R_BAD_ID: &[u8] = b"{\"error\":\"invalid post id\"}";
pub const R_NO_POST: &[u8] = b"{\"error\":\"post not found\"}";
pub const R_NO_BEARER: &[u8] = b"{\"error\":\"missing bearer token\"}";
pub const R_BAD_TOKEN: &[u8] = b"{\"error\":\"invalid or expired token\"}";
pub const R_BAD_PAYLOAD: &[u8] = b"{\"error\":\"invalid token payload\"}";
pub const R_BAD_JSON: &[u8] = b"{\"error\":\"malformed JSON body\"}";
pub const R_NO_BODY: &[u8] = b"{\"error\":\"body is required\"}";
pub const R_LONG_BODY: &[u8] = b"{\"error\":\"body must be at most 500 characters\"}";
pub const R_INTERNAL: &[u8] = b"{\"error\":\"internal server error\"}";

enum AuthErr {
    Missing,
    Bad,
    Payload,
}

impl AuthErr {
    fn body(&self) -> &'static [u8] {
        match self {
            AuthErr::Missing => R_NO_BEARER,
            AuthErr::Bad => R_BAD_TOKEN,
            AuthErr::Payload => R_BAD_PAYLOAD,
        }
    }
}

fn now_s() -> i64 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(_) => 0,
    }
}

fn b64val(c: u8) -> Option<u32> {
    Some(match c {
        b'A'..=b'Z' => (c - b'A') as u32,
        b'a'..=b'z' => (c - b'a') as u32 + 26,
        b'0'..=b'9' => (c - b'0') as u32 + 52,
        b'-' => 62,
        b'_' => 63,
        _ => return None,
    })
}

fn b64url_decode(s: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4 + 2);
    let mut acc: u32 = 0;
    let mut bits = 0u32;
    for &c in s {
        if c == b'=' {
            break;
        }
        let v = b64val(c)?;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

fn parse_positive(s: &[u8]) -> Option<i64> {
    if s.is_empty() || s.len() > 18 {
        return None;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (c - b'0') as i64;
    }
    if v <= 0 {
        None
    } else {
        Some(v)
    }
}

pub struct App {
    db: Db,
    key: HmacKey,
    started: i64,
    scratch: Vec<u8>,
}

impl App {
    pub fn new(db_path: &str, secret: &str) -> App {
        // One process keeps the whole hot set in cache; several consumers of
        // the same 2 GB box each take a quarter of that instead.
        let workers: i32 = std::env::var("FEED_WORKERS")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(5)
            .clamp(1, 16);
        let cache_kb = match std::env::var("FEED_CACHE_KB").ok().and_then(|s| s.parse().ok()) {
            Some(kb) => kb,
            None if workers == 1 => 32768,
            None => (32768 / workers).max(8192),
        };
        App {
            db: Db::open_with_cache(db_path, cache_kb),
            key: HmacKey::new(secret.as_bytes()),
            started: now_s(),
            scratch: Vec::with_capacity(16 * 1024),
        }
    }

    fn jwt(&self, tok: &[u8], now: i64) -> Result<(i64, Vec<u8>), AuthErr> {
        let d1 = tok.iter().position(|&c| c == b'.').ok_or(AuthErr::Bad)?;
        let rest = &tok[d1 + 1..];
        let d2rel = rest.iter().position(|&c| c == b'.').ok_or(AuthErr::Bad)?;
        let d2 = d1 + 1 + d2rel;
        if d1 == 0 || d2 == d1 + 1 || d2 + 1 >= tok.len() {
            return Err(AuthErr::Bad);
        }

        let head = b64url_decode(&tok[..d1]).ok_or(AuthErr::Bad)?;
        let mut hv = [json::Value { raw: &[], ty: json::ABSENT }; 1];
        json::scan(&head, &["alg"], &mut hv).map_err(|_| AuthErr::Bad)?;
        if hv[0].ty != json::STRING || hv[0].raw != b"HS256" {
            return Err(AuthErr::Bad);
        }

        let sig = b64url_decode(&tok[d2 + 1..]).ok_or(AuthErr::Bad)?;
        if sig.len() != 32 {
            return Err(AuthErr::Bad);
        }
        let mac = self.key.mac(&tok[..d2]);
        let sig_arr: [u8; 32] = sig.as_slice().try_into().map_err(|_| AuthErr::Bad)?;
        if !ct_eq32(&mac, &sig_arr) {
            return Err(AuthErr::Bad);
        }

        let pay = b64url_decode(&tok[d1 + 1..d2]).ok_or(AuthErr::Bad)?;
        let mut pv = [
            json::Value { raw: &[], ty: json::ABSENT },
            json::Value { raw: &[], ty: json::ABSENT },
            json::Value { raw: &[], ty: json::ABSENT },
        ];
        json::scan(&pay, &["sub", "username", "exp"], &mut pv).map_err(|_| AuthErr::Bad)?;

        let exp = json::as_int(&pv[2]).ok_or(AuthErr::Bad)?;
        if exp <= now {
            return Err(AuthErr::Bad);
        }
        if pv[0].ty != json::STRING {
            return Err(AuthErr::Payload);
        }
        let uid = parse_positive(pv[0].raw).ok_or(AuthErr::Payload)?;
        if pv[1].ty != json::STRING {
            return Err(AuthErr::Payload);
        }
        let mut name = Vec::new();
        json::unescape(pv[1].raw, &mut name).map_err(|_| AuthErr::Payload)?;
        Ok((uid, name))
    }

    fn authenticate(&self, rq: &Request, now: i64) -> Result<(i64, Vec<u8>), AuthErr> {
        let auth = rq.auth.ok_or(AuthErr::Missing)?;
        if auth.len() < 7 || &auth[..7] != b"Bearer " {
            return Err(AuthErr::Missing);
        }
        self.jwt(&auth[7..], now)
    }

    /// Handle one request. Returns `true` when the connection must be closed.
    pub fn handle(&mut self, rq: &Request, out: &mut Vec<u8>) -> bool {
        if self.db.batch_failed() {
            http::respond(out, 503, b"{\"error\":\"database busy\"}");
            return !rq.keep_alive;
        }
        let now = now_s();
        if rq.method == b"GET" {
            if rq.path == b"/health" {
                if self.db.ping() {
                    let mut body = Vec::with_capacity(64);
                    body.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                    http::put_i64(&mut body, now - self.started);
                    body.push(b'}');
                    http::respond(out, 200, &body);
                } else {
                    http::respond(
                        out,
                        503,
                        b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"query failed\"}",
                    );
                }
                return !rq.keep_alive;
            }
            if rq.path == b"/feed" {
                self.scratch.clear();
                if self.db.feed_json(&mut self.scratch) {
                    http::respond(out, 200, &self.scratch);
                } else {
                    http::respond(out, 500, R_INTERNAL);
                }
                return !rq.keep_alive;
            }
            if let Some((id, with_like)) = post_path(rq.path) {
                if !with_like {
                    match id {
                        IdParse::Invalid => http::respond(out, 400, R_BAD_ID),
                        IdParse::TooBig => http::respond(out, 404, R_NO_POST),
                        IdParse::Ok(id) => {
                            self.scratch.clear();
                            match self.db.post_json(id, &mut self.scratch) {
                                0 => http::respond(out, 200, &self.scratch),
                                1 => http::respond(out, 404, R_NO_POST),
                                _ => http::respond(out, 500, R_INTERNAL),
                            }
                        }
                    }
                    return !rq.keep_alive;
                }
            }
            http::respond(out, 404, R_NOT_FOUND);
            return !rq.keep_alive;
        }

        if rq.method == b"POST" {
            if rq.path == b"/posts" {
                let (uid, name) = match self.authenticate(rq, now) {
                    Ok(v) => v,
                    Err(e) => {
                        http::respond(out, 401, e.body());
                        return !rq.keep_alive;
                    }
                };
                let mut v = [json::Value { raw: &[], ty: json::ABSENT }; 1];
                if json::scan(rq.body, &["body"], &mut v).is_err() {
                    http::respond(out, 400, R_BAD_JSON);
                    return !rq.keep_alive;
                }
                if v[0].ty != json::STRING {
                    http::respond(out, 400, R_NO_BODY);
                    return !rq.keep_alive;
                }
                let mut raw = Vec::with_capacity(v[0].raw.len());
                if json::unescape(v[0].raw, &mut raw).is_err() {
                    http::respond(out, 400, R_BAD_JSON);
                    return !rq.keep_alive;
                }
                let text = match trim_body(&raw) {
                    Ok(t) => t,
                    Err(body) => {
                        http::respond(out, 400, body);
                        return !rq.keep_alive;
                    }
                };
                match self.db.create(uid, text) {
                    Some((id, created)) => {
                        let mut body = Vec::with_capacity(1024);
                        body.extend_from_slice(b"{\"post\":{\"id\":");
                        http::put_i64(&mut body, id);
                        body.extend_from_slice(b",\"body\":");
                        http::put_jstr(&mut body, text);
                        body.extend_from_slice(b",\"created_at\":");
                        http::put_jstr(&mut body, &created);
                        body.extend_from_slice(b",\"author\":");
                        http::put_jstr(&mut body, &name);
                        body.extend_from_slice(b",\"like_count\":0}}");
                        http::respond(out, 201, &body);
                    }
                    None => http::respond(out, 500, R_INTERNAL),
                }
                return !rq.keep_alive;
            }

            if let Some((id, true)) = post_path(rq.path) {
                let (uid, _) = match self.authenticate(rq, now) {
                    Ok(v) => v,
                    Err(e) => {
                        http::respond(out, 401, e.body());
                        return !rq.keep_alive;
                    }
                };
                match id {
                    IdParse::Invalid => http::respond(out, 400, R_BAD_ID),
                    IdParse::TooBig => http::respond(out, 404, R_NO_POST),
                    IdParse::Ok(id) => match self.db.like(uid, id) {
                        LikeResult::Created => {
                            let mut body = Vec::with_capacity(64);
                            body.extend_from_slice(
                                b"{\"liked\":true,\"already_liked\":false,\"post_id\":",
                            );
                            http::put_i64(&mut body, id);
                            body.push(b'}');
                            http::respond(out, 201, &body);
                        }
                        LikeResult::AlreadyLiked => {
                            let mut body = Vec::with_capacity(64);
                            body.extend_from_slice(
                                b"{\"liked\":true,\"already_liked\":true,\"post_id\":",
                            );
                            http::put_i64(&mut body, id);
                            body.push(b'}');
                            http::respond(out, 200, &body);
                        }
                        LikeResult::Missing => http::respond(out, 404, R_NO_POST),
                        LikeResult::Error => http::respond(out, 500, R_INTERNAL),
                    },
                }
                return !rq.keep_alive;
            }
            http::respond(out, 404, R_NOT_FOUND);
            return !rq.keep_alive;
        }

        http::respond(out, 404, R_NOT_FOUND);
        !rq.keep_alive
    }

    pub fn begin_batch(&mut self, write: bool) {
        self.db.batch_begin(write);
    }

    pub fn end_batch(&mut self) -> bool {
        self.db.batch_commit()
    }
}

enum IdParse {
    Invalid,
    TooBig,
    Ok(i64),
}

fn parse_id(s: &[u8]) -> IdParse {
    if s.is_empty() {
        return IdParse::Invalid;
    }
    if s.len() > 18 {
        if s.iter().all(|c| c.is_ascii_digit()) {
            return IdParse::TooBig;
        }
        return IdParse::Invalid;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return IdParse::Invalid;
        }
        v = v * 10 + (c - b'0') as i64;
    }
    if v <= 0 {
        IdParse::Invalid
    } else {
        IdParse::Ok(v)
    }
}

/// Splits `/posts/<id>` and `/posts/<id>/like`.
fn post_path(path: &[u8]) -> Option<(IdParse, bool)> {
    const PRE: &[u8] = b"/posts/";
    if path.len() <= PRE.len() || &path[..PRE.len()] != PRE {
        return None;
    }
    let rest = &path[PRE.len()..];
    match rest.iter().position(|&c| c == b'/') {
        None => Some((parse_id(rest), false)),
        Some(i) => {
            if &rest[i..] == b"/like" {
                Some((parse_id(&rest[..i]), true))
            } else {
                None
            }
        }
    }
}

/// JavaScript's `String.prototype.trim` character set, by code point.
fn trim_space(cp: u32) -> bool {
    matches!(cp, 9..=13 | 32 | 0xA0 | 0x1680 | 0x2000..=0x200A
             | 0x2028 | 0x2029 | 0x202F | 0x205F | 0x3000 | 0xFEFF)
}

fn cp_at(p: &[u8], i: usize) -> (u32, usize) {
    let c = p[i];
    if c < 0x80 {
        (c as u32, 1)
    } else if c < 0xE0 {
        ((((c & 0x1f) as u32) << 6) | (p[i + 1] & 0x3f) as u32, 2)
    } else if c < 0xF0 {
        (
            (((c & 0x0f) as u32) << 12)
                | (((p[i + 1] & 0x3f) as u32) << 6)
                | (p[i + 2] & 0x3f) as u32,
            3,
        )
    } else {
        (
            (((c & 0x07) as u32) << 18)
                | (((p[i + 1] & 0x3f) as u32) << 12)
                | (((p[i + 2] & 0x3f) as u32) << 6)
                | (p[i + 3] & 0x3f) as u32,
            4,
        )
    }
}

/// Trim, then require 1..=500 code points. Returns the error body to use.
fn trim_body(s: &[u8]) -> Result<&[u8], &'static [u8]> {
    let mut a = 0usize;
    let mut b = s.len();
    while a < b {
        let (cp, l) = cp_at(s, a);
        if !trim_space(cp) {
            break;
        }
        a += l;
    }
    while b > a {
        let mut k = b - 1;
        while k > a && (s[k] & 0xC0) == 0x80 {
            k -= 1;
        }
        let (cp, _) = cp_at(s, k);
        if !trim_space(cp) {
            break;
        }
        b = k;
    }
    if a == b {
        return Err(R_NO_BODY);
    }
    let chars = s[a..b].iter().filter(|c| (*c & 0xC0) != 0x80).count();
    if chars > 500 {
        return Err(R_LONG_BODY);
    }
    Ok(&s[a..b])
}
