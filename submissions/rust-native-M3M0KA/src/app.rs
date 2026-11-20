//! The API: routes, validation, JWT verification and response framing.
//! Transport-independent, so the epoll and io_uring loops share it.

use std::time::{SystemTime, UNIX_EPOCH};

use crate::buffer::Output;
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

enum SmallBytes<'a> {
    Stack(&'a [u8]),
    Heap(Vec<u8>),
}

impl<'a> SmallBytes<'a> {
    #[inline]
    fn as_slice(&self) -> &[u8] {
        match self {
            SmallBytes::Stack(s) => s,
            SmallBytes::Heap(v) => v.as_slice(),
        }
    }
}

#[inline]
fn b64url_decode_into(s: &[u8], out: &mut [u8]) -> Option<usize> {
    // JWS uses unpadded base64url (RFC 7515 section 2). Reject impossible
    // lengths and nonzero unused bits instead of accepting aliases/junk.
    if s.len() % 4 == 1 {
        return None;
    }
    let mut len = 0usize;
    let mut acc: u32 = 0;
    let mut bits = 0u32;
    for &c in s {
        let v = b64val(c)?;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out[len] = (acc >> bits) as u8;
            len += 1;
        }
    }
    if bits != 0 && acc & ((1 << bits) - 1) != 0 {
        return None;
    }
    Some(len)
}

fn b64url_decode(s: &[u8]) -> Option<Vec<u8>> {
    if s.len() % 4 == 1 {
        return None;
    }
    let exact = (s.len() / 4) * 3 + match s.len() % 4 { 2 => 1, 3 => 2, _ => 0 };
    let mut out = vec![0u8; exact];
    let n = b64url_decode_into(s, &mut out)?;
    debug_assert_eq!(n, exact);
    Some(out)
}

fn b64url_decode_buf<'a>(s: &[u8], stack: &'a mut [u8]) -> Option<SmallBytes<'a>> {
    if s.len() % 4 == 1 {
        return None;
    }
    let exact = (s.len() / 4) * 3 + match s.len() % 4 { 2 => 1, 3 => 2, _ => 0 };
    if exact <= stack.len() {
        let n = b64url_decode_into(s, &mut stack[..exact])?;
        Some(SmallBytes::Stack(&stack[..n]))
    } else {
        b64url_decode(s).map(SmallBytes::Heap)
    }
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
    pub(crate) db: Db,
    key: HmacKey,
    started: i64,
    scratch: crate::buffer::Buffer,
}

impl App {
    pub fn new(db_path: &str, secret: &str) -> App {
        Self::new_with_worker_hint(db_path, secret, None)
    }

    /// Uses the cache sizing intended for one process, independent of the
    /// epoll worker setting because the L2 candidate has one App/Db owner.
    pub fn new_single_owner(db_path: &str, secret: &str) -> App {
        Self::new_with_worker_hint(db_path, secret, Some(1))
    }

    fn new_with_worker_hint(db_path: &str, secret: &str, worker_hint: Option<i32>) -> App {
        // One process keeps the whole hot set in cache; several consumers of
        // the same 2 GB box each take a quarter of that instead.
        let workers: i32 = worker_hint.unwrap_or_else(|| {
            std::env::var("FEED_WORKERS")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(5)
                .clamp(1, 16)
        });
        let cache_kb = match std::env::var("FEED_CACHE_KB").ok().and_then(|s| s.parse().ok()) {
            Some(kb) => kb,
            None if workers == 1 => 32768,
            None => (32768 / workers).max(8192),
        };
        App {
            db: Db::open_with_cache(db_path, cache_kb),
            key: HmacKey::new(secret.as_bytes()),
            started: now_s(),
            scratch: crate::buffer::Buffer::new(0),
        }
    }

    // Reservation is supplied by the event loop before any request work.
    pub fn output_budget(&mut self, _bytes: usize) {}

    pub fn output_failed(&self) -> bool { false }
    pub fn scratch_capacity(&self) -> usize { 0 }
    pub fn release_scratch(&mut self) {}

    fn jwt_with<R>(key: &HmacKey, tok: &[u8], now: i64, f: impl FnOnce(i64, &[u8]) -> R) -> Result<R, AuthErr> {
        let d1 = tok.iter().position(|&c| c == b'.').ok_or(AuthErr::Bad)?;
        let rest = &tok[d1 + 1..];
        let d2rel = rest.iter().position(|&c| c == b'.').ok_or(AuthErr::Bad)?;
        let d2 = d1 + 1 + d2rel;
        if d1 == 0 || d2 == d1 + 1 || d2 + 1 >= tok.len() {
            return Err(AuthErr::Bad);
        }

        let mut head_buf = [0u8; 128];
        let head = b64url_decode_buf(&tok[..d1], &mut head_buf).ok_or(AuthErr::Bad)?;
        let mut hv = [json::Value { raw: &[], ty: json::ABSENT }; 1];
        json::scan(head.as_slice(), &["alg"], &mut hv).map_err(|_| AuthErr::Bad)?;
        if hv[0].ty != json::STRING {
            return Err(AuthErr::Bad);
        }
        let mut alg_heap = Vec::new();
        let alg = if hv[0].raw.contains(&b'\\') {
            json::unescape(hv[0].raw, &mut alg_heap).map_err(|_| AuthErr::Bad)?;
            alg_heap.as_slice()
        } else {
            hv[0].raw
        };
        if alg != b"HS256" {
            return Err(AuthErr::Bad);
        }

        let mut sig_buf = [0u8; 64];
        let sig = b64url_decode_buf(&tok[d2 + 1..], &mut sig_buf).ok_or(AuthErr::Bad)?;
        let sig_slice = sig.as_slice();
        if sig_slice.len() != 32 {
            return Err(AuthErr::Bad);
        }
        let mac = key.mac(&tok[..d2]);
        let sig_arr: [u8; 32] = sig_slice.try_into().map_err(|_| AuthErr::Bad)?;
        if !ct_eq32(&mac, &sig_arr) {
            return Err(AuthErr::Bad);
        }

        let mut pay_buf = [0u8; 512];
        let pay = b64url_decode_buf(&tok[d1 + 1..d2], &mut pay_buf).ok_or(AuthErr::Bad)?;
        let mut pv = [
            json::Value { raw: &[], ty: json::ABSENT },
            json::Value { raw: &[], ty: json::ABSENT },
            json::Value { raw: &[], ty: json::ABSENT },
        ];
        json::scan(pay.as_slice(), &["sub", "username", "exp"], &mut pv).map_err(|_| AuthErr::Bad)?;

        let exp = json::as_int(&pv[2]).ok_or(AuthErr::Bad)?;
        if exp <= now {
            return Err(AuthErr::Bad);
        }
        if pv[0].ty != json::STRING {
            return Err(AuthErr::Payload);
        }
        let mut sub_heap = Vec::new();
        let sub = if pv[0].raw.contains(&b'\\') {
            json::unescape(pv[0].raw, &mut sub_heap).map_err(|_| AuthErr::Payload)?;
            sub_heap.as_slice()
        } else {
            pv[0].raw
        };
        let uid = parse_positive(sub).ok_or(AuthErr::Payload)?;
        if pv[1].ty != json::STRING {
            return Err(AuthErr::Payload);
        }
        if pv[1].raw.contains(&b'\\') {
            let mut name_heap = Vec::new();
            json::unescape(pv[1].raw, &mut name_heap).map_err(|_| AuthErr::Payload)?;
            Ok(f(uid, &name_heap))
        } else {
            Ok(f(uid, pv[1].raw))
        }
    }

    fn jwt(&self, tok: &[u8], now: i64) -> Result<(i64, Vec<u8>), AuthErr> {
        Self::jwt_with(&self.key, tok, now, |uid, name| (uid, name.to_vec()))
    }

    fn authenticate_with<R>(key: &HmacKey, rq: &Request, now: i64, f: impl FnOnce(i64, &[u8]) -> R) -> Result<R, AuthErr> {
        let auth = rq.auth.ok_or(AuthErr::Missing)?;
        if auth.len() < 7 || &auth[..7] != b"Bearer " {
            return Err(AuthErr::Missing);
        }
        Self::jwt_with(key, &auth[7..], now, f)
    }

    /// Handle one request. Returns `true` when the connection must be closed.
    pub fn handle(&mut self, rq: &Request, out: &mut impl Output) -> bool {
        if self.db.batch_failed() {
            http::respond(out, 503, b"{\"error\":\"database busy\"}");
            return !rq.keep_alive;
        }
        if rq.method == b"GET" {
            if rq.path == b"/health" {
                let now = now_s();
                let started = self.started;
                if self.db.ping() {
                    http::respond_stream(out, 200, 64, |o| {
                        o.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                        http::put_i64(o, now - started);
                        o.push(b'}');
                        true
                    });
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
                let db = &mut self.db;
                if !http::respond_stream(out, 200, 5120, |o| db.feed_json(o)) {
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
                            let db = &mut self.db;
                            let mut status = -1;
                            if !http::respond_stream(out, 200, 384, |o| {
                                status = db.post_json(id, o);
                                status == 0
                            }) {
                                if status == 1 && !out.failed() {
                                    http::respond(out, 404, R_NO_POST);
                                } else {
                                    http::respond(out, 500, R_INTERNAL);
                                }
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
            let now = now_s();
            if rq.path == b"/posts" {
                let mut v = [json::Value { raw: &[], ty: json::ABSENT }; 1];
                let res = Self::authenticate_with(&self.key, rq, now, |uid, name| {
                    if json::scan(rq.body, &["body"], &mut v).is_err() {
                        http::respond(out, 400, R_BAD_JSON);
                        return;
                    }
                    if v[0].ty != json::STRING {
                        http::respond(out, 400, R_NO_BODY);
                        return;
                    }
                    let mut escaped = Vec::new();
                    let raw = if v[0].raw.contains(&b'\\') {
                        if json::unescape(v[0].raw, &mut escaped).is_err() {
                            http::respond(out, 400, R_BAD_JSON);
                            return;
                        }
                        escaped.as_slice()
                    } else {
                        v[0].raw
                    };
                    let text = match trim_body(raw) {
                        Ok(t) => t,
                        Err(body) => {
                            http::respond(out, 400, body);
                            return;
                        }
                    };
                    match self.db.create(uid, text) {
                        Some((id, created)) => {
                            http::respond_stream(out, 201, 256 + text.len() + name.len(), |o| {
                                o.extend_from_slice(b"{\"post\":{\"id\":");
                                http::put_i64(o, id);
                                o.extend_from_slice(b",\"body\":");
                                http::put_jstr(o, text);
                                o.extend_from_slice(b",\"created_at\":");
                                http::put_jstr(o, &created);
                                o.extend_from_slice(b",\"author\":");
                                http::put_jstr(o, name);
                                o.extend_from_slice(b",\"like_count\":0}}");
                                true
                            });
                        }
                        None => http::respond(out, 500, R_INTERNAL),
                    }
                });
                if let Err(e) = res {
                    http::respond(out, 401, e.body());
                }
                return !rq.keep_alive;
            }

            if let Some((id, true)) = post_path(rq.path) {
                let uid = match Self::authenticate_with(&self.key, rq, now, |uid, _| uid) {
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
                            http::respond_stream(out, 201, 64, |o| {
                                o.extend_from_slice(b"{\"liked\":true,\"already_liked\":false,\"post_id\":");
                                http::put_i64(o, id);
                                o.push(b'}');
                                true
                            });
                        }
                        LikeResult::AlreadyLiked => {
                            http::respond_stream(out, 200, 64, |o| {
                                o.extend_from_slice(b"{\"liked\":true,\"already_liked\":true,\"post_id\":");
                                http::put_i64(o, id);
                                o.push(b'}');
                                true
                            });
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

// Called only on decoded strings from a successful, UTF-8-validated JSON scan.
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

#[cfg(test)]
mod auth_buf_tests {
    use super::{b64url_decode, b64url_decode_buf, SmallBytes};

    #[test]
    fn stack_and_heap_decode_match_and_validate() {
        let mut small = [0u8; 8];
        for (enc, exp) in [
            (b"".as_slice(), b"".as_slice()),
            (b"Zg", b"f"),
            (b"Zm8", b"fo"),
            (b"Zm9v", b"foo"),
            (b"YWJjZGVmZ2g", b"abcdefgh"),
        ] {
            let decoded = b64url_decode_buf(enc, &mut small).unwrap();
            assert!(matches!(decoded, SmallBytes::Stack(_)));
            assert_eq!(decoded.as_slice(), exp);
            assert_eq!(b64url_decode(enc).as_deref(), Some(exp));
        }
        // Longer than 8 decoded bytes triggers heap fallback transparently.
        let long_enc = b"YWJjZGVmZ2hpams"; // "abcdefghijk" (11 bytes)
        let decoded = b64url_decode_buf(long_enc, &mut small).unwrap();
        assert!(matches!(decoded, SmallBytes::Heap(_)));
        assert_eq!(decoded.as_slice(), b"abcdefghijk");
        for bad in [b"A".as_slice(), b"Zg=", b"Zh", b"Zm9", b"YWJjZGVmZ2hpamx"] {
            assert!(b64url_decode_buf(bad, &mut small).is_none());
            assert!(b64url_decode(bad).is_none());
        }
    }
}
