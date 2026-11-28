// MIT. Codec routines adapted from goose-iouring-ahmadsh2007 and sstadick PR 1.
import src.model;
import src.json;
namespace jwt;

struct Key { inner: u32[8], outer: u32[8] }

let K256: u32[64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

let H256: u32[8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

fn rotr(x: u32, n: u32) -> u32 { return (x >> n) | (x << (32 - n)); }

fn compress(h: u32[8]&, blk: const u8[:], at: i64) {
    var w: u32[64] = [0; 64];
    for i in 16 {
        let j = at + i * 4;
        w[i] = (blk[j] as u32 << 24) | (blk[j + 1] as u32 << 16) | (blk[j + 2] as u32 << 8) | blk[j + 3];
    }
    for i in 16..64 {
        let a = w[i - 15];
        let b = w[i - 2];
        let s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3);
        let s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    var a = h[0]; var b = h[1]; var c = h[2]; var d = h[3];
    var e = h[4]; var f = h[5]; var g = h[6]; var k = h[7];
    for i in 64 {
        let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        let ch = (e & f) ^ (~e & g);
        let t1 = k + s1 + ch + K256[i] + w[i];
        let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let t2 = s0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

// before counts the bytes in complete blocks already absorbed into h.
fn sha_finish(h: u32[8]&, msg: const u8[:], before: i64, out: u8[32]&) {
    var i = 0;
    while i + 64 <= msg.len { compress(h, msg, i); i += 64; }
    var tail: u8[128] = [0; 128];
    let rest = msg.len - i;
    for k in rest { tail[k] = msg[i + k]; }
    tail[rest] = 0x80;
    let blocks = if rest + 9 <= 64 { 1 } else { 2 };
    let bits = ((before + msg.len) * 8) as u64;
    let end = blocks * 64;
    for k in 8 { tail[end - 1 - k] = (bits >> (k * 8) as u64) as! u8; }
    compress(h, tail, 0);
    if blocks == 2 { compress(h, tail, 64); }
    for k in 8 {
        out[k * 4] = (h[k] >> 24) as! u8;
        out[k * 4 + 1] = (h[k] >> 16) as! u8;
        out[k * 4 + 2] = (h[k] >> 8) as! u8;
        out[k * 4 + 3] = h[k] as! u8;
    }
}

fn key(secret: const u8[:]) -> Key {
    var result = Key { inner: H256, outer: H256 };
    var key: u8[64] = [0; 64];
    if secret.len > 64 {
        var h = H256;
        var d: u8[32] = [0; 32];
        sha_finish(h, secret, 0, d);
        for k in 32 { key[k] = d[k]; }
    } else {
        for k in secret.len { key[k] = secret[k]; }
    }
    var ip: u8[64] = [0; 64];
    var op: u8[64] = [0; 64];
    for k in 64 { ip[k] = key[k] ^ 0x36; op[k] = key[k] ^ 0x5c; }
    compress(result.inner, ip, 0);
    compress(result.outer, op, 0);
    return result;
}

fn hmac(key: Key&, msg: const u8[:], out: u8[32]&) {
    var h = key.inner;
    var inner: u8[32] = [0; 32];
    sha_finish(h, msg, 64, inner);
    var g = key.outer;
    sha_finish(g, inner, 64, out);
}

fn b64val(c: u8) -> i64 {
    if c >= 'A' && c <= 'Z' { return c - 'A'; }
    if c >= 'a' && c <= 'z' { return (c - 'a') as i64 + 26; }
    if c >= '0' && c <= '9' { return (c - '0') as i64 + 52; }
    if c == '-' { return 62; }
    if c == '_' { return 63; }
    return -1;
}

// The unused bits of the last base64url character must be zero.
fn b64url(s: const u8[:], out: u8[>..]&) -> bool {
    if s.len % 4 == 1 { return false; }
    var i = 0;
    while i + 4 <= s.len {
        let a = b64val(s[i]); let b = b64val(s[i + 1]); let c = b64val(s[i + 2]); let d = b64val(s[i + 3]);
        if (a | b | c | d) < 0 { return false; }
        let v = (a << 18) | (b << 12) | (c << 6) | d;
        out.push((v >> 16) as! u8);
        out.push((v >> 8) as! u8);
        out.push(v as! u8);
        i += 4;
    }
    let r = s.len - i;
    if r == 0 { return true; }
    var v = 0;
    for k in r {
        let x = b64val(s[i + k]);
        if x < 0 { return false; }
        v = v | (x << (18 - 6 * k));
    }
    let unused = if r == 2 { v & 0xffff } else { v & 0xff };
    if unused != 0 { return false; }
    out.push((v >> 16) as! u8);
    if r == 3 { out.push((v >> 8) as! u8); }
    return true;
}

fn verify(key: Key&, authorization: const u8[:], now: f64, want_name: bool,
          jwt_head: u8[>..]&, jwt_body: u8[>..]&, scratch: u8[>..]&,
          scratch2: u8[>..]&, username: u8[>..]&) -> model::Auth {
    if !starts_with(authorization, "Bearer ") {
        return model::Auth.Missing;
    }
    let tok = authorization[7..];
    var d1 = -1;
    var d2 = -1;
    var dots = 0;
    for c, i in tok {
        if c == '.' {
            dots++;
            if dots == 1 { d1 = i; } else if dots == 2 { d2 = i; }
        }
    }
    if dots != 2 { return model::Auth.Invalid; }
    jwt_head.clear();
    if !b64url(tok[d2 + 1..], jwt_head) || jwt_head.len != 32 {
        return model::Auth.Invalid;
    }
    var mac: u8[32] = [0; 32];
    hmac(key, tok[..d2], mac);
    var diff: u8 = 0;
    for k in 32 { diff |= mac[k] ^ jwt_head[k]; }
    if diff != 0 { return model::Auth.Invalid; }
    jwt_head.clear();
    if !b64url(tok[..d1], jwt_head) { return model::Auth.Invalid; }
    var alg_ok = false;
    var p = 0;
    let hs = jwt_head[..];
    let hst = json::object_members(hs, p, scratch) { key =>
        if key == "alg" {
            if p < hs.len && hs[p] == '"' {
                let s, e, escd, ok = json::read_string(hs, p);
                if !ok { return -1 from json::object_members; }
                if escd {
                    scratch2.clear();
                    json::unescape(hs[s..e], scratch2);
                    alg_ok = scratch2 == "HS256";
                } else {
                    alg_ok = hs[s..e] == "HS256";
                }
            } else {
                if !json::skip_value(hs, p) { return -1 from json::object_members; }
                alg_ok = false;
            }
        } else {
            if !json::skip_value(hs, p) { return -1 from json::object_members; }
        }
    };
    if hst != 1 || !alg_ok { return model::Auth.Invalid; }
    jwt_body.clear();
    if !b64url(tok[d1 + 1..d2], jwt_body) { return model::Auth.Invalid; }
    let ps = jwt_body[..];
    var exp = 0.0;
    var has_exp = false;
    var sub_s = -1; var sub_e = -1; var sub_esc = false;
    var nm_s = -1; var nm_e = -1; var nm_esc = false;
    p = 0;
    let pst = json::object_members(ps, p, scratch) { key =>
        if key == "exp" {
            let c = if p < ps.len { ps[p] } else { 0 };
            if c == '-' || (c >= '0' && c <= '9') {
                let v, ok = json::read_number(ps, p);
                if !ok { return -1 from json::object_members; }
                exp = v;
                has_exp = true;
            } else {
                if !json::skip_value(ps, p) { return -1 from json::object_members; }
                has_exp = false;
            }
        } else if key == "sub" {
            if p < ps.len && ps[p] == '"' {
                let s, e, escd, ok = json::read_string(ps, p);
                if !ok { return -1 from json::object_members; }
                sub_s = s; sub_e = e; sub_esc = escd;
            } else {
                if !json::skip_value(ps, p) { return -1 from json::object_members; }
                sub_s = -1;
            }
        } else if key == "username" {
            if p < ps.len && ps[p] == '"' {
                let s, e, escd, ok = json::read_string(ps, p);
                if !ok { return -1 from json::object_members; }
                nm_s = s; nm_e = e; nm_esc = escd;
            } else {
                if !json::skip_value(ps, p) { return -1 from json::object_members; }
                nm_s = -1;
            }
        } else {
            if !json::skip_value(ps, p) { return -1 from json::object_members; }
        }
    };
    if pst != 1 { return model::Auth.Invalid; }
    if !has_exp || !(now < exp) { return model::Auth.Invalid; }
    if sub_s < 0 || nm_s < 0 { return model::Auth.InvalidClaims; }
    var uid = 0;
    if sub_esc {
        scratch2.clear();
        json::unescape(ps[sub_s..sub_e], scratch2);
        uid = model::positive_int(scratch2);
    } else {
        uid = model::positive_int(ps[sub_s..sub_e]);
    }
    if uid <= 0 { return model::Auth.InvalidClaims; }
    if want_name {
        if nm_esc { json::unescape(ps[nm_s..nm_e], username); }
        else { username.append(ps[nm_s..nm_e]); }
    }
    return model::Auth.Verified { user_id: uid };
}
