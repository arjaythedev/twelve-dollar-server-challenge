// Feed API: one io_uring event loop, SQLite in-process, group commit per completion batch.
// Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT. See README.md.
use io_uring::types::{BufRingEntry, CancelBuilder, DestinationSlot, Fd, Fixed, SubmitArgs, Timespec};
use io_uring::{cqueue, opcode, squeue, IoUring};
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr;
use std::sync::atomic::{AtomicU16, Ordering};

// ---------- SQLite (static amalgamation, see build.rs) ----------
type Db = c_void;
type Stmt = c_void;
extern "C" {
    fn sqlite3_initialize() -> c_int;
    fn sqlite3_open_v2(f: *const c_char, db: *mut *mut Db, flags: c_int, vfs: *const c_char) -> c_int;
    fn sqlite3_busy_timeout(db: *mut Db, ms: c_int) -> c_int;
    fn sqlite3_exec(db: *mut Db, sql: *const c_char, cb: *const c_void, arg: *mut c_void, err: *mut *mut c_char) -> c_int;
    fn sqlite3_prepare_v3(db: *mut Db, sql: *const c_char, n: c_int, flags: u32, st: *mut *mut Stmt, tail: *mut *const c_char) -> c_int;
    fn sqlite3_step(s: *mut Stmt) -> c_int;
    fn sqlite3_reset(s: *mut Stmt) -> c_int;
    fn sqlite3_bind_int64(s: *mut Stmt, i: c_int, v: i64) -> c_int;
    fn sqlite3_bind_text(s: *mut Stmt, i: c_int, p: *const c_char, n: c_int, destructor: isize) -> c_int;
    fn sqlite3_column_int64(s: *mut Stmt, i: c_int) -> i64;
    fn sqlite3_column_text(s: *mut Stmt, i: c_int) -> *const u8;
    fn sqlite3_column_bytes(s: *mut Stmt, i: c_int) -> c_int;
    fn sqlite3_changes(db: *mut Db) -> c_int;
    fn sqlite3_get_autocommit(db: *mut Db) -> c_int;
    fn sqlite3_errmsg(db: *mut Db) -> *const c_char;
    fn sqlite3_wal_checkpoint_v2(db: *mut Db, name: *const c_char, mode: c_int, log: *mut c_int, done: *mut c_int) -> c_int;
}
const ROW: c_int = 100;
const DONE: c_int = 101;

fn col_text<'a>(s: *mut Stmt, i: c_int) -> &'a [u8] {
    unsafe {
        let p = sqlite3_column_text(s, i);
        let n = sqlite3_column_bytes(s, i) as usize;
        if p.is_null() { &[] } else { std::slice::from_raw_parts(p, n) }
    }
}
fn col_i64(s: *mut Stmt, i: c_int) -> i64 { unsafe { sqlite3_column_int64(s, i) } }
fn step(s: *mut Stmt) -> c_int { unsafe { sqlite3_step(s) } }
fn wstep(s: *mut Stmt, txn_writes: &mut bool) -> c_int { // a write inside the batch transaction
    let rc = step(s);
    if rc == ROW || rc == DONE { *txn_writes = true; }
    rc
}
fn reset(s: *mut Stmt) { unsafe { sqlite3_reset(s); } }

const MAXREQ: usize = 64 * 1024; // header + body cap per request
const BATCH_OUT: usize = 4 << 20; // response bytes held per batch until its commit
const NBUF: usize = 4096; // provided receive buffers per ring
const BSZ: usize = 2048;
const MAXRINGS: usize = 32;
const ACCEPTS: u32 = 16;
// user_data: low 3 bits tag the operation. A send's user_data is its Box<Sop> pointer (tag 0); a
// receive's is the connection: T_RECV | ring << 3 | slot << 8 | generation << 32.
const T_SEND: u64 = 0;
const T_ACCEPT: u64 = 1;
const T_RECV: u64 = 2;
const T_NOP: u64 = 3;
const T_EVFD: u64 = 4;
fn ring_of(who: u64) -> usize { (who >> 3 & 31) as usize }
fn slot_of(who: u64) -> u32 { (who >> 8 & 0xffffff) as u32 }

// ---------- response arena ----------
struct Out { a: Vec<u8>, bs: usize }
impl Out {
    #[inline] fn put(&mut self, s: &[u8]) { self.a.extend_from_slice(s) }
    fn putu(&mut self, mut v: u64) {
        let mut b = [0u8; 20];
        let mut i = 20;
        loop { i -= 1; b[i] = b'0' + (v % 10) as u8; v /= 10; if v == 0 { break; } }
        self.put(&b[i..]);
    }
    fn putjs(&mut self, s: &[u8]) { // JSON string contents, escaped
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let mut run = 0;
        for (i, &c) in s.iter().enumerate() {
            if c >= 0x20 && c != b'"' && c != b'\\' { continue; }
            self.a.extend_from_slice(&s[run..i]);
            run = i + 1;
            match c {
                b'"' => self.put(b"\\\""),
                b'\\' => self.put(b"\\\\"),
                b'\n' => self.put(b"\\n"),
                b'\r' => self.put(b"\\r"),
                b'\t' => self.put(b"\\t"),
                _ => self.put(&[b'\\', b'u', b'0', b'0', HEX[(c >> 4) as usize], HEX[(c & 15) as usize]]),
            }
        }
        self.a.extend_from_slice(&s[run..]);
    }
    // head with a blank Content-Length that finish() fills in
    fn begin(&mut self, status: u16, close: bool) {
        self.put(match status {
            200 => &b"HTTP/1.1 200 OK\r\n"[..],
            201 => b"HTTP/1.1 201 Created\r\n",
            400 => b"HTTP/1.1 400 Bad Request\r\n",
            401 => b"HTTP/1.1 401 Unauthorized\r\n",
            404 => b"HTTP/1.1 404 Not Found\r\n",
            503 => b"HTTP/1.1 503 Service Unavailable\r\n",
            _ => b"HTTP/1.1 500 Internal Server Error\r\n",
        });
        if close { self.put(b"Connection: close\r\n"); }
        self.put(b"Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
        self.bs = self.a.len();
    }
    fn finish(&mut self) {
        let mut n = self.a.len() - self.bs;
        let mut p = self.bs - 4;
        loop { p -= 1; self.a[p] = b'0' + (n % 10) as u8; n /= 10; if n == 0 { break; } }
    }
    fn err(&mut self, status: u16, msg: &str, close: bool) {
        self.begin(status, close);
        self.put(b"{\"error\":\"");
        self.put(msg.as_bytes());
        self.put(b"\"}");
        self.finish();
    }
}

// ---------- SHA-256 / HMAC ----------
const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];
#[derive(Clone, Copy)]
struct Sha { h: [u32; 8], n: u64, b: [u8; 64], bl: usize }
impl Sha {
    fn new() -> Sha {
        Sha { h: [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19], n: 0, b: [0; 64], bl: 0 }
    }
    fn block(h: &mut [u32; 8], p: &[u8]) {
        let mut w = [0u32; 64];
        for i in 0..16 { w[i] = u32::from_be_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]); }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut k] = *h;
        for i in 0..64 {
            let t1 = k.wrapping_add(e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25))
                .wrapping_add((e & f) ^ (!e & g)).wrapping_add(K[i]).wrapping_add(w[i]);
            let t2 = (a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22)).wrapping_add((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d.wrapping_add(t1); d = c; c = b; b = a; a = t1.wrapping_add(t2);
        }
        for (x, v) in h.iter_mut().zip([a, b, c, d, e, f, g, k]) { *x = x.wrapping_add(v); }
    }
    fn update(&mut self, mut p: &[u8]) {
        self.n += p.len() as u64;
        if self.bl > 0 {
            let t = (64 - self.bl).min(p.len());
            self.b[self.bl..self.bl + t].copy_from_slice(&p[..t]);
            self.bl += t;
            p = &p[t..];
            if self.bl == 64 { let b = self.b; Sha::block(&mut self.h, &b); self.bl = 0; }
        }
        while p.len() >= 64 { Sha::block(&mut self.h, &p[..64]); p = &p[64..]; }
        if !p.is_empty() { self.b[..p.len()].copy_from_slice(p); self.bl = p.len(); }
    }
    fn finish(mut self) -> [u8; 32] {
        let bits = self.n * 8;
        self.update(&[0x80]);
        let z = [0u8; 64];
        let pad = if self.bl <= 56 { 56 - self.bl } else { 120 - self.bl };
        self.update(&z[..pad]);
        self.update(&bits.to_be_bytes());
        let mut out = [0u8; 32];
        for i in 0..8 { out[4 * i..4 * i + 4].copy_from_slice(&self.h[i].to_be_bytes()); }
        out
    }
}
// HMAC key schedule: hash states after absorbing key^ipad / key^opad
struct Hmac { inner: Sha, outer: Sha }
impl Hmac {
    fn new(key: &[u8]) -> Hmac {
        let mut k = [0u8; 64];
        if key.len() > 64 { let mut s = Sha::new(); s.update(key); k[..32].copy_from_slice(&s.finish()); } else { k[..key.len()].copy_from_slice(key); }
        let mut inner = Sha::new();
        let mut outer = Sha::new();
        inner.update(&k.map(|b| b ^ 0x36));
        outer.update(&k.map(|b| b ^ 0x5c));
        Hmac { inner, outer }
    }
    fn mac(&self, m: &[u8]) -> [u8; 32] {
        let mut s = self.inner;
        s.update(m);
        let ih = s.finish();
        let mut o = self.outer;
        o.update(&ih);
        o.finish()
    }
}

// ---------- base64url ----------
const fn b64tab() -> [i8; 256] {
    let mut t = [-1i8; 256];
    let a = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut i = 0;
    while i < 64 { t[a[i] as usize] = i as i8; i += 1; }
    t
}
static B64: [i8; 256] = b64tab();
fn b64dec(s: &[u8], out: &mut [u8]) -> Option<usize> {
    if s.len() % 4 == 1 { return None; }
    let (mut acc, mut bits, mut o) = (0u32, 0, 0);
    for &c in s {
        let v = B64[c as usize];
        if v < 0 { return None; }
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 { bits -= 8; out[o] = (acc >> bits) as u8; o += 1; }
    }
    Some(o)
}

// ---------- JSON (strict, RFC 8259) ----------
fn ws(s: &[u8], mut p: usize) -> usize {
    while p < s.len() && matches!(s[p], b' ' | b'\t' | b'\n' | b'\r') { p += 1; }
    p
}
fn hexv(c: u8) -> u32 {
    match c { b'0'..=b'9' => (c - b'0') as u32, b'a'..=b'f' => (c - b'a' + 10) as u32, b'A'..=b'F' => (c - b'A' + 10) as u32, _ => 0x10000 }
}
fn hex4(s: &[u8]) -> Option<u32> {
    let v = hexv(s[0]) << 12 | hexv(s[1]) << 8 | hexv(s[2]) << 4 | hexv(s[3]);
    if s[..4].iter().any(|&c| hexv(c) > 15) { None } else { Some(v & 0xffff) }
}
fn utf8_len(p: &[u8]) -> usize { // length of one valid UTF-8 sequence, 0 if invalid
    let c = p[0];
    let cont = |i: usize| p.len() > i && p[i] & 0xc0 == 0x80;
    match c {
        0x00..=0x7f => 1,
        0xc2..=0xdf => if cont(1) { 2 } else { 0 },
        0xe0..=0xef => {
            if !cont(1) || !cont(2) || (c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f) { 0 } else { 3 }
        }
        0xf0..=0xf4 => {
            if !cont(1) || !cont(2) || !cont(3) || (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f) { 0 } else { 4 }
        }
        _ => 0,
    }
}
// Parses the string starting at s[p] == '"', decoding UTF-8 into out when given.
// Returns the index after the closing quote, None on a syntax error.
fn jstr(s: &[u8], p: usize, mut out: Option<&mut Vec<u8>>) -> Option<usize> {
    let mut p = p + 1;
    while p < s.len() {
        let c = s[p];
        if c == b'"' { return Some(p + 1); }
        if c < 0x20 { return None; }
        if c == b'\\' {
            p += 1;
            if p >= s.len() { return None; }
            let ch = match s[p] {
                b'"' => b'"', b'\\' => b'\\', b'/' => b'/', b'b' => 8, b'f' => 12, b'n' => b'\n', b'r' => b'\r', b't' => b'\t',
                b'u' => {
                    if s.len() - p < 5 { return None; }
                    let mut cp = hex4(&s[p + 1..])?;
                    p += 4;
                    if (0xd800..=0xdbff).contains(&cp) && s.len() - p >= 7 && s[p + 1] == b'\\' && s[p + 2] == b'u' {
                        let lo = hex4(&s[p + 3..])?;
                        if (0xdc00..=0xdfff).contains(&lo) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                    }
                    // lone surrogate: U+FFFD, as JS TextEncoder does
                    let ch = char::from_u32(cp).unwrap_or('\u{fffd}');
                    if let Some(o) = out.as_deref_mut() { o.extend_from_slice(ch.encode_utf8(&mut [0; 4]).as_bytes()); }
                    p += 1;
                    continue;
                }
                _ => return None,
            };
            if let Some(o) = out.as_deref_mut() { o.push(ch); }
            p += 1;
            continue;
        }
        let l = utf8_len(&s[p..]);
        if l == 0 { return None; }
        if let Some(o) = out.as_deref_mut() { o.extend_from_slice(&s[p..p + l]); }
        p += l;
    }
    None
}
fn digits(s: &[u8], mut p: usize) -> usize { while p < s.len() && s[p].is_ascii_digit() { p += 1; } p }
fn jnumber(s: &[u8], mut p: usize) -> Option<usize> {
    if p < s.len() && s[p] == b'-' { p += 1; }
    if p >= s.len() { return None; }
    if s[p] == b'0' { p += 1; } else if s[p].is_ascii_digit() { p = digits(s, p); } else { return None; }
    if p < s.len() && s[p] == b'.' {
        p += 1;
        if p >= s.len() || !s[p].is_ascii_digit() { return None; }
        p = digits(s, p);
    }
    if p < s.len() && (s[p] == b'e' || s[p] == b'E') {
        p += 1;
        if p < s.len() && (s[p] == b'+' || s[p] == b'-') { p += 1; }
        if p >= s.len() || !s[p].is_ascii_digit() { return None; }
        p = digits(s, p);
    }
    Some(p)
}
fn jlit(s: &[u8], p: usize, lit: &[u8]) -> Option<usize> { if s[p..].starts_with(lit) { Some(p + lit.len()) } else { None } }
fn jvalue(s: &[u8], p: usize, depth: u32) -> Option<usize> { // validates and skips one value
    if depth > 64 { return None; }
    let mut p = ws(s, p);
    if p >= s.len() { return None; }
    match s[p] {
        b'"' => jstr(s, p, None),
        b'{' => {
            p = ws(s, p + 1);
            if p < s.len() && s[p] == b'}' { return Some(p + 1); }
            loop {
                p = ws(s, p);
                if p >= s.len() || s[p] != b'"' { return None; }
                p = ws(s, jstr(s, p, None)?);
                if p >= s.len() || s[p] != b':' { return None; }
                p = ws(s, jvalue(s, p + 1, depth + 1)?);
                if p >= s.len() { return None; }
                match s[p] { b',' => p += 1, b'}' => return Some(p + 1), _ => return None }
            }
        }
        b'[' => {
            p = ws(s, p + 1);
            if p < s.len() && s[p] == b']' { return Some(p + 1); }
            loop {
                p = ws(s, jvalue(s, p, depth + 1)?);
                if p >= s.len() { return None; }
                match s[p] { b',' => p += 1, b']' => return Some(p + 1), _ => return None }
            }
        }
        b't' => jlit(s, p, b"true"),
        b'f' => jlit(s, p, b"false"),
        b'n' => jlit(s, p, b"null"),
        _ => jnumber(s, p),
    }
}
// Validates a whole document. For a top-level object it records the span of the last value of each
// wanted key (the last duplicate wins, as in JSON.parse). Returns false if the document is invalid.
fn jdoc(s: &[u8], keys: &[&[u8]], spans: &mut [Option<(usize, usize)>]) -> bool {
    spans.iter_mut().for_each(|x| *x = None);
    let mut p = ws(s, 0);
    if p < s.len() && s[p] == b'{' {
        p = ws(s, p + 1);
        if p < s.len() && s[p] == b'}' { p += 1; } else {
            let mut kb = Vec::new();
            loop {
                p = ws(s, p);
                if p >= s.len() || s[p] != b'"' { return false; }
                let ks = p;
                let Some(ke) = jstr(s, p, None) else { return false };
                let raw = &s[ks + 1..ke - 1];
                let key: &[u8] = if raw.contains(&b'\\') { kb.clear(); jstr(s, ks, Some(&mut kb)); &kb } else { raw };
                p = ws(s, ke);
                if p >= s.len() || s[p] != b':' { return false; }
                let vs = ws(s, p + 1);
                let Some(ve) = jvalue(s, vs, 1) else { return false };
                for (i, k) in keys.iter().enumerate() { if *k == key { spans[i] = Some((vs, ve)); } }
                p = ws(s, ve);
                if p >= s.len() { return false; }
                match s[p] { b',' => p += 1, b'}' => { p += 1; break; } _ => return false }
            }
        }
    } else {
        match jvalue(s, p, 0) { Some(e) => p = e, None => return false }
    }
    ws(s, p) == s.len()
}

// ---------- auth ----------
const BAD: &str = "invalid or expired token";
const PAYLOAD: &str = "invalid token payload";
// Returns the user id and fills uname, or the message to send with the 401.
fn auth(h: Option<&[u8]>, hm: &Hmac, now: i64, uname: &mut Vec<u8>) -> Result<i64, &'static str> {
    let t = match h { Some(h) if h.starts_with(b"Bearer ") => &h[7..], _ => return Err("missing bearer token") };
    let mut dots = t.iter().enumerate().filter(|(_, &c)| c == b'.').map(|(i, _)| i);
    let (Some(d1), Some(d2), None) = (dots.next(), dots.next(), dots.next()) else { return Err(BAD) };
    let (mut sig, mut hdr, mut pl) = ([0u8; 48], [0u8; 512], [0u8; 2048]);
    if t.len() - d2 - 1 != 43 || b64dec(&t[d2 + 1..], &mut sig) != Some(32) { return Err(BAD); }
    let mac = hm.mac(&t[..d2]);
    if sig[..32].iter().zip(mac.iter()).fold(0u8, |a, (x, y)| a | (x ^ y)) != 0 { return Err(BAD); }
    if d1 > 680 || d2 - d1 - 1 > 2700 { return Err(BAD); }
    let (Some(hn), Some(pn)) = (b64dec(&t[..d1], &mut hdr), b64dec(&t[d1 + 1..d2], &mut pl)) else { return Err(BAD) };
    let (hdr, pl) = (&hdr[..hn], &pl[..pn]);
    let mut hs = [None];
    if !jdoc(hdr, &[b"alg"], &mut hs) { return Err(BAD); }
    match hs[0] { Some((a, b)) if &hdr[a..b] == b"\"HS256\"" => {}
        Some((a, _)) if hdr[a] == b'"' => { let mut v = Vec::new(); jstr(hdr, a, Some(&mut v)); if v != b"HS256" { return Err(BAD); } }
        _ => return Err(BAD) }
    let mut ps = [None; 4];
    if !jdoc(pl, &[b"exp", b"nbf", b"sub", b"username"], &mut ps) || ps[0].is_none() { return Err(BAD); } // exp is required
    for i in 0..2 {
        if let Some((a, b)) = ps[i] { // exp / nbf must be numbers, checked against now
            if pl[a] != b'-' && !pl[a].is_ascii_digit() { return Err(BAD); }
            let v: f64 = std::str::from_utf8(&pl[a..b]).ok().and_then(|x| x.parse().ok()).ok_or(BAD)?;
            if if i == 0 { now as f64 >= v } else { (now as f64) < v } { return Err(BAD); }
        }
    }
    let (Some((sa, _)), Some((ua, _))) = (ps[2], ps[3]) else { return Err(PAYLOAD) };
    if pl[sa] != b'"' || pl[ua] != b'"' { return Err(PAYLOAD); }
    let mut sb = Vec::new();
    jstr(pl, sa, Some(&mut sb));
    if sb.is_empty() || sb.len() > 15 || !sb.iter().all(u8::is_ascii_digit) { return Err(PAYLOAD); }
    let v = sb.iter().fold(0i64, |v, &d| v * 10 + (d - b'0') as i64);
    if v <= 0 { return Err(PAYLOAD); }
    uname.clear();
    jstr(pl, ua, Some(uname));
    Ok(v)
}

// ---------- request helpers ----------
fn parse_id(s: &[u8]) -> i64 { // positive decimal integer, else -1 (huge ids map to not found)
    if s.is_empty() { return -1; }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() { return -1; }
        v = if v < 100_000_000_000_000_000 { v * 10 + (c - b'0') as i64 } else { i64::MAX };
    }
    if v > 0 { v } else { -1 }
}
fn jsws_at(s: &[u8]) -> usize { // byte length of JS String.trim whitespace at s[0], or 0
    let c = s[0];
    if c == b' ' || (9..=13).contains(&c) { return 1; }
    if s.len() >= 2 && c == 0xc2 && s[1] == 0xa0 { return 2; }
    if s.len() >= 3 {
        let v = (c as u32) << 16 | (s[1] as u32) << 8 | s[2] as u32;
        if v == 0xe19a80 || (0xe28080..=0xe2808a).contains(&v) || matches!(v, 0xe280a8 | 0xe280a9 | 0xe280af | 0xe2819f | 0xe38080 | 0xefbbbf) { return 3; }
    }
    0
}
fn jsws_before(s: &[u8]) -> usize {
    let n = s.len();
    let c = s[n - 1];
    if c == b' ' || (9..=13).contains(&c) { return 1; }
    if n >= 2 && s[n - 2] == 0xc2 && c == 0xa0 { return 2; }
    if n >= 3 && jsws_at(&s[n - 3..]) == 3 { return 3; }
    0
}
fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    let p = unsafe { libc::memmem(h.as_ptr() as *const c_void, h.len(), n.as_ptr() as *const c_void, n.len()) };
    if p.is_null() { None } else { Some(p as usize - h.as_ptr() as usize) }
}
fn pos(h: &[u8], c: u8) -> Option<usize> { h.iter().position(|&x| x == c) }

// ---------- server state ----------
// A response in flight; who is the connection's receive tag, so a late completion can tell whether
// the connection it belongs to is still the same one.
struct Sop { who: u64, d: Vec<u8>, off: usize }
#[derive(Default)]
struct Conn {
    inb: Vec<u8>, // partial request, or input held while a send is in flight
    q: Vec<u8>,   // responses queued behind the send in flight
    gen: u32,
    open: bool, closing: bool, sending: bool,
}
// Accepted sockets live in each ring's registered file table, not in the process descriptor table.
// A table can't be larger than RLIMIT_NOFILE, so more rings are added as tables fill up.
struct Ring {
    r: IoUring,
    c: Vec<Conn>, n: u32, pend: u32, // open connections, accepts in flight
    br: *mut BufRingEntry, tail: u16, bufs: Vec<u8>,
}

struct Srv {
    db: *mut Db,
    feed: *mut Stmt, fids: *mut Stmt, flikes: *mut Stmt, frows: *mut Stmt, post: *mut Stmt, ins: *mut Stmt, like: *mut Stmt, exists: *mut Stmt, one: *mut Stmt,
    begin: *mut Stmt, commit: *mut Stmt, rollback: *mut Stmt,
    hm: Hmac,
    o: Out,
    segs: Vec<(u64, usize, usize)>,
    deferred: Vec<u64>, rearm: Vec<u64>,
    arena_full: bool,
    in_txn: bool, txn_writes: bool, batch_failed: bool,
    rings: Vec<Ring>, tsize: u32, naccepts: u32,
    lfd: c_int, efd: c_int, evbuf: Box<u64>,
    started: i64, now: i64,
    txt: Vec<u8>, un: Vec<u8>, tmp: Vec<u8>,
}

const ISE: &str = "internal server error";

impl Srv {
    fn txn(&mut self) -> bool { // opens the batch transaction on first use
        if !self.in_txn { let rc = step(self.begin); reset(self.begin); self.in_txn = rc == DONE; }
        self.in_txn
    }
    fn db_fail(&mut self) { // a failed statement may have rolled the whole batch back
        if self.in_txn && unsafe { sqlite3_get_autocommit(self.db) } != 0 {
            self.in_txn = false;
            if self.txn_writes { self.batch_failed = true; }
            self.txn_writes = false;
        }
    }
    fn post_row(&mut self, s: *mut Stmt, likes: i64) {
        let o = &mut self.o;
        o.put(b"{\"id\":"); o.putu(col_i64(s, 0) as u64);
        o.put(b",\"body\":\""); o.putjs(col_text(s, 1));
        o.put(b"\",\"created_at\":\""); o.put(col_text(s, 2));
        o.put(b"\",\"author\":\""); o.putjs(col_text(s, 3));
        o.put(b"\",\"like_count\":"); o.putu(likes as u64); o.put(b"}");
    }
    // The feed's newest 20 ids usually sit in a narrow id range. Then one scan of that range in likes and
    // one in posts replace the 20 per-row count and join lookups. Returns Some(true) when the feed was
    // written, Some(false) to fall back to the reference query (ids too spread out), None on a database error.
    fn feed_range(&mut self, close: bool) -> Option<bool> {
        let (mut ids, mut n) = ([0i64; 20], 0);
        let rc = loop { let rc = step(self.fids); if rc != ROW { break rc; } ids[n] = col_i64(self.fids, 0); n += 1; };
        reset(self.fids);
        if rc != DONE { return None; }
        let ids = &ids[..n];
        let (lo, hi) = (ids.iter().min().copied().unwrap_or(0), ids.iter().max().copied().unwrap_or(0));
        if hi - lo >= 256 { return Some(false); }
        let mut slot = [usize::MAX; 256];
        for (i, &id) in ids.iter().enumerate() { slot[(id - lo) as usize] = i; }
        let mut likes = [0i64; 20];
        unsafe { sqlite3_bind_int64(self.flikes, 1, lo); sqlite3_bind_int64(self.flikes, 2, hi); }
        let rc = loop { let rc = step(self.flikes); if rc != ROW { break rc; } if let Some(l) = likes.get_mut(slot[(col_i64(self.flikes, 0) - lo) as usize]) { *l += 1; } };
        reset(self.flikes);
        if rc != DONE { return None; }
        let mark = self.o.a.len();
        self.o.begin(200, close);
        self.o.put(b"{\"posts\":[");
        let rows = self.o.a.len();
        let (mut span, mut found) = ([(0, 0); 20], 0);
        unsafe { sqlite3_bind_int64(self.frows, 1, lo); sqlite3_bind_int64(self.frows, 2, hi); }
        let rc = loop {
            let rc = step(self.frows);
            if rc != ROW { break rc; }
            let k = slot[(col_i64(self.frows, 0) - lo) as usize];
            if k == usize::MAX { continue; }
            let at = self.o.a.len();
            self.post_row(self.frows, likes[k]);
            span[k] = (at - rows, self.o.a.len() - rows);
            found += 1;
        };
        reset(self.frows);
        if rc != DONE || found != n { self.o.a.truncate(mark); return if rc != DONE { None } else { Some(false) }; }
        self.tmp.clear(); // back into feed order
        self.tmp.extend_from_slice(&self.o.a[rows..]);
        self.o.a.truncate(rows);
        for (i, &(a, b)) in span[..n].iter().enumerate() { if i > 0 { self.o.put(b","); } self.o.put(&self.tmp[a..b]); }
        self.o.put(b"]}");
        self.o.finish();
        Some(true)
    }

    fn handle(&mut self, m: &[u8], path: &[u8], authh: Option<&[u8]>, body: &[u8], close: bool) {
        let path = match pos(path, b'?') { Some(q) => &path[..q], None => path };
        let (get, post) = (m == b"GET", m == b"POST");
        if get && path == b"/feed" {
            if !self.txn() { return self.o.err(500, ISE, close); }
            match self.feed_range(close) { Some(true) => return, None => { self.db_fail(); return self.o.err(500, ISE, close); } _ => {} }
            let mark = self.o.a.len();
            self.o.begin(200, close);
            self.o.put(b"{\"posts\":[");
            let mut i = 0;
            let rc = loop {
                let rc = step(self.feed);
                if rc != ROW { break rc; }
                if i > 0 { self.o.put(b","); }
                i += 1;
                self.post_row(self.feed, col_i64(self.feed, 4));
            };
            reset(self.feed);
            if rc != DONE { self.db_fail(); self.o.a.truncate(mark); return self.o.err(500, ISE, close); }
            self.o.put(b"]}");
            return self.o.finish();
        }
        if path.len() > 7 && path.starts_with(b"/posts/") {
            let seg = &path[7..];
            let sl = pos(seg, b'/');
            if get && sl.is_none() {
                let id = parse_id(seg);
                if id < 0 { return self.o.err(400, "invalid post id", close); }
                if !self.txn() { return self.o.err(500, ISE, close); }
                unsafe { sqlite3_bind_int64(self.post, 1, id); }
                let mark = self.o.a.len();
                let rc = step(self.post);
                if rc == ROW { self.o.begin(200, close); self.o.put(b"{\"post\":"); self.post_row(self.post, col_i64(self.post, 4)); self.o.put(b"}"); self.o.finish(); }
                else if rc == DONE { self.o.err(404, "post not found", close); }
                else { self.db_fail(); self.o.a.truncate(mark); self.o.err(500, ISE, close); }
                return reset(self.post);
            }
            if let (true, Some(sl)) = (post, sl) {
                if &seg[sl..] == b"/like" {
                    let uid = match auth(authh, &self.hm, self.now, &mut self.un) { Ok(u) => u, Err(e) => return self.o.err(401, e, close) };
                    let id = parse_id(&seg[..sl]);
                    if id < 0 { return self.o.err(400, "invalid post id", close); }
                    if !self.txn() { return self.o.err(500, ISE, close); }
                    unsafe { sqlite3_bind_int64(self.like, 1, uid); sqlite3_bind_int64(self.like, 2, id); }
                    let rc = wstep(self.like, &mut self.txn_writes);
                    reset(self.like);
                    if rc != DONE { self.db_fail(); return self.o.err(500, ISE, close); }
                    let created = unsafe { sqlite3_changes(self.db) } > 0;
                    if !created {
                        unsafe { sqlite3_bind_int64(self.exists, 1, id); }
                        let rc = step(self.exists);
                        reset(self.exists);
                        if rc == DONE { return self.o.err(404, "post not found", close); }
                        if rc != ROW { self.db_fail(); return self.o.err(500, ISE, close); }
                    }
                    self.o.begin(if created { 201 } else { 200 }, close);
                    self.o.put(if created { &b"{\"liked\":true,\"already_liked\":false,\"post_id\":"[..] } else { b"{\"liked\":true,\"already_liked\":true,\"post_id\":" });
                    self.o.putu(id as u64);
                    self.o.put(b"}");
                    return self.o.finish();
                }
            }
        }
        if post && path == b"/posts" {
            let uid = match auth(authh, &self.hm, self.now, &mut self.un) { Ok(u) => u, Err(e) => return self.o.err(401, e, close) };
            let mut w = [None];
            if !jdoc(body, &[b"body"], &mut w) { return self.o.err(400, "malformed JSON body", close); }
            let Some((vs, _)) = w[0] else { return self.o.err(400, "body is required", close) };
            if body[vs] != b'"' { return self.o.err(400, "body is required", close); }
            self.txt.clear();
            if jstr(body, vs, Some(&mut self.txt)).is_none() { return self.o.err(400, "malformed JSON body", close); }
            let mut s: &[u8] = &self.txt;
            while !s.is_empty() { let l = jsws_at(s); if l == 0 { break; } s = &s[l..]; }
            while !s.is_empty() { let l = jsws_before(s); if l == 0 { break; } s = &s[..s.len() - l]; }
            if s.is_empty() { return self.o.err(400, "body is required", close); }
            if s.iter().filter(|&&c| c & 0xc0 != 0x80).count() > 500 { return self.o.err(400, "body must be at most 500 characters", close); }
            // txn() inlined: s still borrows self.txt
            if !self.in_txn { let rc = step(self.begin); reset(self.begin); self.in_txn = rc == DONE; }
            if !self.in_txn { return self.o.err(500, ISE, close); }
            unsafe {
                sqlite3_bind_int64(self.ins, 1, uid);
                sqlite3_bind_text(self.ins, 2, s.as_ptr() as *const c_char, s.len() as c_int, 0);
            }
            let rc = wstep(self.ins, &mut self.txn_writes);
            if rc != ROW { reset(self.ins); self.db_fail(); return self.o.err(500, ISE, close); }
            let mark = self.o.a.len();
            let o = &mut self.o;
            o.begin(201, close);
            o.put(b"{\"post\":{\"id\":"); o.putu(col_i64(self.ins, 0) as u64);
            o.put(b",\"body\":\""); o.putjs(s);
            o.put(b"\",\"created_at\":\""); o.put(col_text(self.ins, 1));
            o.put(b"\",\"author\":\""); o.putjs(&self.un); o.put(b"\",\"like_count\":0}}"); o.finish();
            let rc = step(self.ins); // run RETURNING to completion so the insert finishes
            reset(self.ins);
            if rc != DONE { self.db_fail(); self.o.a.truncate(mark); self.o.err(500, ISE, close); }
            return;
        }
        if get && path == b"/health" {
            let rc = step(self.one);
            reset(self.one);
            if rc == ROW {
                self.o.begin(200, close);
                self.o.put(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                self.o.putu((self.now - self.started) as u64);
                self.o.put(b"}");
            } else {
                self.o.begin(503, close);
                self.o.put(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                let msg = unsafe { CStr::from_ptr(sqlite3_errmsg(self.db)) };
                self.o.putjs(msg.to_bytes());
                self.o.put(b"\"}");
            }
            return self.o.finish();
        }
        self.o.err(404, "not found", close)
    }

    // Serves every complete request in buf. Returns (bytes consumed, closing), or None to drop the connection.
    fn serve(&mut self, buf: &[u8]) -> Option<(usize, bool)> {
        let mut used = 0;
        let mut closing = false;
        while used < buf.len() && !closing {
            let p = &buf[used..];
            if self.o.a.len() + 2 * MAXREQ > BATCH_OUT { self.arena_full = true; break; } // the rest waits for the next batch
            let Some(he) = find(p, b"\r\n\r\n") else { if p.len() > MAXREQ { return None; } break };
            let le = pos(&p[..he + 2], b'\r')?;
            let line = &p[..le];
            let sp1 = pos(line, b' ')?;
            let sp2 = sp1 + 1 + pos(&line[sp1 + 1..], b' ')?;
            let (m, path) = (&line[..sp1], &line[sp1 + 1..sp2]);
            let mut close = &line[sp2 + 1..] == b"HTTP/1.0";
            let (mut authh, mut clen, mut expect) = (None, 0usize, false);
            let mut h = le + 2;
            while h < he + 2 {
                let hl = h + pos(&p[h..he + 2], b'\r')?;
                if let Some(c) = pos(&p[h..hl], b':') {
                    let k = &p[h..h + c];
                    let mut v = &p[h + c + 1..hl];
                    while let [b' ' | b'\t', rest @ ..] = v { v = rest; }
                    while let [rest @ .., b' ' | b'\t'] = v { v = rest; }
                    if k.eq_ignore_ascii_case(b"content-length") {
                        clen = 0;
                        for &d in v {
                            if !d.is_ascii_digit() { return None; }
                            clen = clen * 10 + (d - b'0') as usize;
                            if clen > MAXREQ { return None; }
                        }
                    } else if k.eq_ignore_ascii_case(b"authorization") { authh = Some(v); }
                    else if k.eq_ignore_ascii_case(b"connection") {
                        if v.eq_ignore_ascii_case(b"close") { close = true; } else if v.eq_ignore_ascii_case(b"keep-alive") { close = false; }
                    } else if k.eq_ignore_ascii_case(b"expect") { expect = v.eq_ignore_ascii_case(b"100-continue"); }
                    else if k.eq_ignore_ascii_case(b"transfer-encoding") { return None; } // chunked uploads aren't supported
                }
                h = hl + 2;
            }
            let body = he + 4;
            if p.len() - body < clen {
                if expect && body == p.len() { self.o.put(b"HTTP/1.1 100 Continue\r\n\r\n"); }
                break;
            }
            self.handle(m, path, authh, &p[body..body + clen], close);
            if close { closing = true; }
            used += body + clen;
        }
        Some((used, closing))
    }


    fn conn(&mut self, who: u64) -> Option<&mut Conn> { // None once that connection has closed
        let c = &mut self.rings[ring_of(who)].c[slot_of(who) as usize];
        if c.open && c.gen == (who >> 32) as u32 { Some(c) } else { None }
    }
    fn sqe(&mut self, k: usize, e: &[squeue::Entry]) { // queues e in one submission
        let r = &mut self.rings[k].r;
        while unsafe { r.submission().push_multiple(e) }.is_err() { let _ = r.submit(); }
    }
    fn conn_close(&mut self, who: u64) {
        let (k, slot) = (ring_of(who), slot_of(who));
        let r = &mut self.rings[k];
        let c = &mut r.c[slot as usize];
        *c = Conn { gen: c.gen.wrapping_add(1), ..Conn::default() }; // a send still in flight frees itself when it completes
        r.n -= 1;
        self.sqe(k, &[ // end the pending receive, then free the slot
            opcode::AsyncCancel2::new(CancelBuilder::fd(Fixed(slot)).all()).build().flags(squeue::Flags::IO_HARDLINK).user_data(T_NOP),
            opcode::Close::new(Fixed(slot)).build().user_data(T_NOP),
        ]);
    }
    fn arm_recv(&mut self, who: u64) { // multishot: one completion per chunk, into a provided buffer
        self.sqe(ring_of(who), &[opcode::RecvMulti::new(Fixed(slot_of(who)), 0).build().user_data(who)]);
    }
    fn send_op(&mut self, o: Box<Sop>) {
        let e = opcode::Send::new(Fixed(slot_of(o.who)), o.d[o.off..].as_ptr(), (o.d.len() - o.off) as u32).flags(libc::MSG_NOSIGNAL);
        let k = ring_of(o.who);
        self.sqe(k, &[e.build().user_data(Box::into_raw(o) as u64)]);
    }
    // One send in flight per connection keeps responses in order; later ones queue behind it.
    // Each send owns a copy of its bytes, so the arena is reused at once and a slow reader can't pin it.
    fn send_bytes(&mut self, who: u64, off: usize, len: usize) {
        let Srv { o, rings, .. } = self;
        let c = &mut rings[ring_of(who)].c[slot_of(who) as usize];
        let p = &o.a[off..off + len];
        if c.sending { return c.q.extend_from_slice(p); }
        c.sending = true;
        let d = p.to_vec();
        self.send_op(Box::new(Sop { who, d, off: 0 }));
    }

    // Serves buf; returns the bytes consumed, None once the connection is done.
    fn process(&mut self, who: u64, buf: &[u8]) -> Option<usize> {
        let start = self.o.a.len();
        self.arena_full = false;
        let Some((used, closing)) = self.serve(buf) else { self.o.a.truncate(start); self.conn_close(who); return None };
        let end = self.o.a.len();
        if end > start { self.segs.push((who, start, end - start)); }
        self.conn(who)?.closing = closing;
        if closing {
            if end == start { self.conn_close(who); }
            return None;
        }
        if used < buf.len() && self.arena_full { self.deferred.push(who); }
        Some(used)
    }
    fn process_owned(&mut self, who: u64, mut v: Vec<u8>) { // leftover bytes (a partial request) stay on the connection
        if let Some(used) = self.process(who, &v) {
            if used < v.len() { v.drain(..used); self.conn(who).unwrap().inb = v; }
        }
    }
    fn on_data(&mut self, who: u64, p: &[u8]) {
        let c = self.conn(who).unwrap();
        if c.inb.is_empty() && !c.sending {
            if let Some(used) = self.process(who, p) {
                if used < p.len() { self.conn(who).unwrap().inb = p[used..].to_vec(); }
            }
            return;
        }
        c.inb.extend_from_slice(p);
        if c.inb.len() > 4 * MAXREQ { return self.conn_close(who); } // keeps sending but won't read its replies
        if !c.sending { let v = std::mem::take(&mut c.inb); self.process_owned(who, v); }
    }
    fn send_done(&mut self, mut o: Box<Sop>, res: i32) {
        let who = o.who;
        let alive = self.conn(who).is_some();
        if alive && res > 0 {
            o.off += res as usize;
            if o.off < o.d.len() { return self.send_op(o); } // short send: the rest
        }
        drop(o);
        if !alive { return; }
        if res <= 0 { return self.conn_close(who); }
        let c = self.conn(who).unwrap();
        if !c.q.is_empty() { let d = std::mem::take(&mut c.q); return self.send_op(Box::new(Sop { who, d, off: 0 })); }
        c.sending = false;
        if c.closing { self.conn_close(who); }
        else if !c.inb.is_empty() { let v = std::mem::take(&mut c.inb); self.process_owned(who, v); } // input that arrived while sending
    }

    // Commits the batch, then sends every response produced in it.
    fn flush_batch(&mut self) {
        if self.in_txn {
            let rc = step(self.commit);
            reset(self.commit);
            if rc != DONE {
                step(self.rollback);
                reset(self.rollback);
                if self.txn_writes { self.batch_failed = true; }
            }
            self.in_txn = false;
            self.txn_writes = false;
        }
        let segs = std::mem::take(&mut self.segs);
        for &(who, off, len) in &segs {
            if self.conn(who).is_none() { continue; }
            if self.batch_failed { self.conn_close(who); continue; } // the batch wasn't committed: never claim it was
            self.send_bytes(who, off, len);
        }
        self.segs = segs;
        self.segs.clear();
        self.o.a.clear();
        self.batch_failed = false;
    }

    // ---------- rings ----------
    fn arm_evfd(&mut self) { // extra rings signal this eventfd; ring 0 waits on it
        let e = opcode::Read::new(Fd(self.efd), &mut *self.evbuf as *mut u64 as *mut u8, 8).build().user_data(T_EVFD);
        self.sqe(0, &[e]);
    }
    fn ring_init(&mut self) -> std::io::Result<()> {
        let k = self.rings.len();
        let mut b = IoUring::builder();
        b.setup_submit_all().setup_single_issuer().setup_cqsize(16384);
        if k == 0 { b.setup_defer_taskrun(); } // ring 0 is the one the loop waits on, so its completions run in that wait
        let r = b.build(4096)?;
        r.submitter().register_files_sparse(self.tsize)?;
        let br = unsafe { libc::mmap(ptr::null_mut(), NBUF * 16, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_PRIVATE | libc::MAP_ANONYMOUS, -1, 0) };
        if br == libc::MAP_FAILED { return Err(std::io::Error::last_os_error()); }
        unsafe { r.submitter().register_buf_ring_with_flags(br as u64, NBUF as u16, 0, 0)?; }
        let mut ring = Ring { r, c: (0..self.tsize).map(|_| Conn::default()).collect(), n: 0, pend: 0, br: br as *mut BufRingEntry, tail: 0, bufs: vec![0; NBUF * BSZ] };
        for i in 0..NBUF { ring.buf_add(i as u16); }
        if k > 0 {
            if self.efd < 0 { self.efd = unsafe { libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK) }; }
            ring.r.submitter().register_eventfd(self.efd)?;
            eprintln!("{} connections: added ring {k}", k as u32 * (self.tsize - 16));
        }
        self.rings.push(ring);
        if k == 1 { self.arm_evfd(); }
        Ok(())
    }
    // Accepts are single-shot, a few in flight, on the first ring with room. An accept that finds its
    // table full has already taken the connection and can only drop it, so tables are never filled to the
    // last slot (closes still in flight may hold a few) and a multishot accept can't be stopped in time.
    fn arm_accepts(&mut self) {
        let mut k = 0;
        while self.naccepts < ACCEPTS && k < MAXRINGS {
            if k == self.rings.len() && self.ring_init().is_err() { return; } // retried after the next event
            let r = &mut self.rings[k];
            if r.n + r.pend + 16 >= self.tsize { k += 1; continue; }
            r.pend += 1;
            self.naccepts += 1;
            let e = opcode::Accept::new(Fd(self.lfd), ptr::null_mut(), ptr::null_mut()).file_index(Some(DestinationSlot::auto_target()));
            self.sqe(k, &[e.build().user_data(T_ACCEPT)]);
        }
    }
    fn on_cqe(&mut self, k: usize, ud: u64, res: i32, f: u32) {
        match ud & 7 {
            T_SEND => self.send_done(unsafe { Box::from_raw(ud as *mut Sop) }, res),
            T_ACCEPT => {
                let r = &mut self.rings[k];
                r.pend -= 1;
                self.naccepts -= 1;
                if res >= 0 {
                    let c = &mut r.c[res as usize];
                    c.open = true;
                    r.n += 1;
                    let who = T_RECV | (k as u64) << 3 | (res as u64) << 8 | (c.gen as u64) << 32;
                    self.arm_recv(who);
                }
            }
            T_RECV => {
                let alive = self.conn(ud).is_some();
                if let Some(bid) = cqueue::buffer_select(f) {
                    let p = unsafe { self.rings[k].bufs.as_ptr().add(bid as usize * BSZ) };
                    if alive && res > 0 { self.on_data(ud, unsafe { std::slice::from_raw_parts(p, res as usize) }); }
                    self.rings[k].buf_add(bid);
                }
                if cqueue::more(f) { return; }
                let Some(c) = self.conn(ud) else { return };
                if res > 0 || res == -libc::ENOBUFS { self.rearm.push(ud); } // rearmed once buffers are back
                else if c.sending { c.closing = true; } // EOF: finish the reply in flight first
                else { self.conn_close(ud); }
            }
            T_EVFD => self.arm_evfd(),
            _ => {}
        }
    }

    fn run(&mut self) -> ! {
        let (mut cqes, mut seen) = (Vec::new(), 0);
        loop {
            for who in std::mem::take(&mut self.rearm) { if self.conn(who).is_some() { self.arm_recv(who); } }
            self.arm_accepts();
            for r in &mut self.rings[1..] {
                if !r.r.submission().is_empty() || r.r.submission().cq_overflow() { let _ = r.r.submit(); }
            }
            // Busy (the last pass saw 4+ completions): wait for up to 128 or 6 ms, so a batch, its commit and
            // its sends cover more requests per wake-up. The target is capped at 40% of open connections so a
            // few clients without think time never sit out the timeout.
            let rc = if seen >= 4 {
                let open: u32 = self.rings.iter().map(|r| r.n).sum();
                let ts = Timespec::new().nsec(6_000_000);
                self.rings[0].r.submitter().submit_with_args((open * 2 / 5 + 1).min(128) as usize, &SubmitArgs::new().timespec(&ts))
            } else { self.rings[0].r.submit_and_wait(1) };
            if let Err(e) = rc {
                if !matches!(e.raw_os_error(), Some(libc::EINTR | libc::EBUSY | libc::ETIME)) { eprintln!("io_uring_enter: {e}"); std::process::exit(1); }
            }
            self.now = now();
            seen = 0;
            for k in 0..self.rings.len() {
                cqes.extend(self.rings[k].r.completion().map(|e| (e.user_data(), e.result(), e.flags())));
                seen += cqes.len();
                for &(ud, res, f) in &cqes { self.on_cqe(k, ud, res, f); }
                cqes.clear();
            }
            self.flush_batch();
            while !self.deferred.is_empty() { // requests held back by a full arena
                for who in std::mem::take(&mut self.deferred) {
                    let Some(c) = self.conn(who) else { continue };
                    if !c.inb.is_empty() && !c.sending { let v = std::mem::take(&mut c.inb); self.process_owned(who, v); }
                }
                self.flush_batch();
            }
        }
    }
}

impl Ring {
    fn buf_add(&mut self, bid: u16) { // hands a receive buffer (back) to the kernel
        unsafe {
            let e = &mut *self.br.add((self.tail & (NBUF as u16 - 1)) as usize);
            e.set_addr(self.bufs.as_ptr().add(bid as usize * BSZ) as u64);
            e.set_len(BSZ as u32);
            e.set_bid(bid);
            self.tail = self.tail.wrapping_add(1);
            (*(BufRingEntry::tail(self.br) as *const AtomicU16)).store(self.tail, Ordering::Release);
        }
    }
}

fn now() -> i64 { unsafe { libc::time(ptr::null_mut()) as i64 } }

// Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
// the length of the fsync, so a helper process does them and the event loop never waits on the disk.
fn checkpointer(path: &CStr) -> ! {
    unsafe {
        libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
        let mut db = ptr::null_mut();
        if sqlite3_open_v2(path.as_ptr(), &mut db, 0x2 /* READWRITE */, ptr::null()) != 0 { std::process::exit(1); }
        sqlite3_busy_timeout(db, 100);
        sqlite3_exec(db, c"PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;".as_ptr(), ptr::null(), ptr::null_mut(), ptr::null_mut());
        loop {
            let (mut log, mut done) = (0, 0);
            sqlite3_wal_checkpoint_v2(db, ptr::null(), 0 /* PASSIVE */, &mut log, &mut done);
            libc::usleep(if log > 0 && done < log { 1000 } else { 10000 });
        }
    }
}

fn main() {
    let env = |k: &str| std::env::var(k).ok();
    let (Some(path), Some(secret)) = (env("SQLITE_PATH"), env("JWT_SECRET")) else {
        eprintln!("SQLITE_PATH and JWT_SECRET are required");
        std::process::exit(1);
    };
    let host = env("HOST").unwrap_or_else(|| "0.0.0.0".into());
    let host: std::net::Ipv4Addr = host.parse().unwrap_or_else(|_| { eprintln!("bad HOST {host}"); std::process::exit(1) });
    let port: u16 = env("PORT").and_then(|p| p.parse().ok()).unwrap_or(3000);
    let args: Vec<String> = std::env::args().collect();
    let cpath = CString::new(path.clone()).unwrap();
    let mut rl: libc::rlimit = unsafe { std::mem::zeroed() };
    unsafe {
        libc::signal(libc::SIGCHLD, libc::SIG_IGN); // the checkpointer is reaped automatically
        libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
        if rl.rlim_cur < rl.rlim_max { rl.rlim_cur = rl.rlim_max; libc::setrlimit(libc::RLIMIT_NOFILE, &rl); }
        sqlite3_initialize(); // built with SQLITE_OMIT_AUTOINIT
    }
    if args.get(1).is_some_and(|a| a == "--ckpt") { checkpointer(&cpath); }

    // ---- database ----
    let mut db = ptr::null_mut();
    if unsafe { sqlite3_open_v2(cpath.as_ptr(), &mut db, 0x2 /* READWRITE */, ptr::null()) } != 0 {
        eprintln!("open {path} failed");
        std::process::exit(1);
    }
    unsafe { sqlite3_busy_timeout(db, 5000); }
    let exec = |sql: &str| unsafe { let c = CString::new(sql).unwrap(); sqlite3_exec(db, c.as_ptr(), ptr::null(), ptr::null_mut(), ptr::null_mut()) };
    if exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000; SELECT count(*) FROM users WHERE id = 1;") != 0 {
        eprintln!("pragma failed: {:?}", unsafe { CStr::from_ptr(sqlite3_errmsg(db)) });
        std::process::exit(1);
    }
    // The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
    // for a long time), so the WAL stays bounded at about 256 MB.
    exec("PRAGMA wal_autocheckpoint=65536;");
    let prep = |sql: &str| -> *mut Stmt {
        let c = CString::new(sql).unwrap();
        let mut s = ptr::null_mut();
        if unsafe { sqlite3_prepare_v3(db, c.as_ptr(), -1, 1 /* PERSISTENT */, &mut s, ptr::null_mut()) } != 0 {
            eprintln!("prepare {sql}: {:?}", unsafe { CStr::from_ptr(sqlite3_errmsg(db)) });
            std::process::exit(1);
        }
        s
    };
    let cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
    let t = now();
    let mut srv = Srv {
        db,
        feed: prep(&format!("{cols}ORDER BY p.created_at DESC, p.id DESC LIMIT 20")),
        fids: prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20"),
        flikes: prep("SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?"),
        frows: prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?"),
        post: prep(&format!("{cols}WHERE p.id = ?")),
        ins: prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"),
        like: prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING"),
        exists: prep("SELECT 1 FROM posts WHERE id = ?"),
        one: prep("SELECT 1"),
        begin: prep("BEGIN"),
        commit: prep("COMMIT"),
        rollback: prep("ROLLBACK"),
        hm: Hmac::new(secret.as_bytes()),
        o: Out { a: Vec::with_capacity(BATCH_OUT), bs: 0 },
        segs: Vec::with_capacity(4096),
        deferred: Vec::new(), rearm: Vec::new(),
        arena_full: false,
        in_txn: false, txn_writes: false, batch_failed: false,
        rings: Vec::new(), tsize: rl.rlim_cur.min(1 << 20) as u32, naccepts: 0, // the kernel caps a table at RLIMIT_NOFILE
        lfd: -1, efd: -1, evbuf: Box::new(0),
        started: t, now: t,
        txt: Vec::with_capacity(MAXREQ), un: Vec::with_capacity(256), tmp: Vec::new(),
    };
    step(srv.feed); // touches the hot pages before the first request
    reset(srv.feed);
    if unsafe { libc::fork() } == 0 {
        let argv = [c"server".as_ptr(), c"--ckpt".as_ptr(), ptr::null()];
        unsafe { libc::execv(c"/proc/self/exe".as_ptr(), argv.as_ptr()); libc::_exit(1); }
    }

    unsafe {
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        let one: c_int = 1;
        let sz = std::mem::size_of::<c_int>() as u32;
        libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, &one as *const _ as *const c_void, sz);
        libc::setsockopt(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, &one as *const _ as *const c_void, sz); // accepted sockets inherit it
        let mut a: libc::sockaddr_in = std::mem::zeroed();
        a.sin_family = libc::AF_INET as u16;
        a.sin_port = port.to_be();
        a.sin_addr.s_addr = u32::from(host).to_be();
        if libc::bind(fd, &a as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in>() as u32) != 0 || libc::listen(fd, 65535) != 0 {
            eprintln!("bind/listen: {}", std::io::Error::last_os_error());
            std::process::exit(1);
        }
        srv.lfd = fd;
    }
    if let Err(e) = srv.ring_init() {
        eprintln!("io_uring setup failed: {e} (needs Linux 6.1+ with io_uring enabled)");
        std::process::exit(1);
    }
    srv.run()
}
