// Feed API: one epoll loop, SQLite in-process, group commit per loop iteration.
// Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT. See README.md for the design.
const std = @import("std");
const linux = std.os.linux;
const c = @cImport(@cInclude("sqlite3.h"));

const MAXFD = 65536;
const RBUF = 64 * 1024;
const MAXREQ = 64 * 1024; // header + body cap per request
const BATCH_OUT = 4 << 20; // response bytes held per loop iteration until its commit
const gpa = std.heap.c_allocator;

const Conn = struct {
    in: []u8 = &.{}, // partial request carried between reads (rare)
    inlen: usize = 0,
    out: []u8 = &.{}, // unsent response bytes after a short write (rare)
    outoff: usize = 0,
    open: bool = false,
    closing: bool = false,
};

var conns: []Conn = undefined;
var epfd: i32 = 0;
var nconns: usize = 0;
var db: ?*c.sqlite3 = null;
var st_feed: ?*c.sqlite3_stmt = null;
var st_fids: ?*c.sqlite3_stmt = null;
var st_flikes: ?*c.sqlite3_stmt = null;
var st_frows: ?*c.sqlite3_stmt = null;
var st_post: ?*c.sqlite3_stmt = null;
var st_ins: ?*c.sqlite3_stmt = null;
var st_like: ?*c.sqlite3_stmt = null;
var st_exists: ?*c.sqlite3_stmt = null;
var st_one: ?*c.sqlite3_stmt = null;
var st_begin: ?*c.sqlite3_stmt = null;
var st_beginimm: ?*c.sqlite3_stmt = null;
var st_commit: ?*c.sqlite3_stmt = null;
var st_rollback: ?*c.sqlite3_stmt = null;
var started: i64 = 0;
var now_s: i64 = 0;

// ---------- raw syscalls ----------
fn failed(rc: usize) bool {
    return rc > std.math.maxInt(usize) - 4095;
}
fn errno(rc: usize) usize {
    return 0 -% rc;
}
const EAGAIN = 11;
const EINTR = 4;
extern "c" fn signal(sig: c_int, handler: usize) usize;
extern "c" fn fork() c_int;
extern "c" fn pipe2(fds: *[2]c_int, flags: c_int) c_int;
extern "c" fn execv(path: [*:0]const u8, argv: [*:null]const ?[*:0]const u8) c_int;
const O_CLOEXEC = 0o2000000;

// ---------- output: one arena per loop iteration, sent after COMMIT ----------
var arena: []u8 = undefined;
var alen: usize = 0;
const Seg = struct { fd: i32, off: usize, len: usize };
var segs: []Seg = undefined;
var nsegs: usize = 0;
var deferred: []i32 = undefined;
var ndef: usize = 0;
var arena_full = false;
var in_txn = false;
var txn_writes = false;
var batch_failed = false;

inline fn put(b: []const u8) void {
    @memcpy(arena[alen..][0..b.len], b);
    alen += b.len;
}
fn putu(v0: u64) void {
    var b: [24]u8 = undefined;
    var i: usize = 24;
    var v = v0;
    while (true) {
        i -= 1;
        b[i] = '0' + @as(u8, @intCast(v % 10));
        v /= 10;
        if (v == 0) break;
    }
    put(b[i..]);
}
const hexd = "0123456789abcdef";
fn putjs(s: []const u8) void { // JSON string contents, escaped
    var run: usize = 0;
    for (s, 0..) |ch, i| {
        if (ch >= 0x20 and ch != '"' and ch != '\\') continue;
        put(s[run..i]);
        run = i + 1;
        switch (ch) {
            '"' => put("\\\""),
            '\\' => put("\\\\"),
            '\n' => put("\\n"),
            '\r' => put("\\r"),
            '\t' => put("\\t"),
            else => put(&[_]u8{ '\\', 'u', '0', '0', hexd[ch >> 4], hexd[ch & 15] }),
        }
    }
    put(s[run..]);
}

// response head with a blank Content-Length that finish() fills in
var body_start: usize = 0;
var cl_end: usize = 0;
fn beginResp(status: u16, close: bool) void {
    put(switch (status) {
        200 => "HTTP/1.1 200 OK\r\n",
        201 => "HTTP/1.1 201 Created\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n",
        401 => "HTTP/1.1 401 Unauthorized\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n",
        503 => "HTTP/1.1 503 Service Unavailable\r\n",
        else => "HTTP/1.1 500 Internal Server Error\r\n",
    });
    if (close) put("Connection: close\r\n");
    put("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
    cl_end = alen - 4;
    body_start = alen;
}
fn finish() void {
    var n = alen - body_start;
    var p = cl_end;
    while (true) {
        p -= 1;
        arena[p] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
        if (n == 0) break;
    }
}
fn err(status: u16, msg: []const u8, close: bool) void {
    beginResp(status, close);
    put("{\"error\":\"");
    put(msg);
    put("\"}");
    finish();
}

// ---------- SHA-256 / HMAC ----------
// A plain SHA-256 loop: without CPU SHA instructions (x86-64-v2 has none) std.crypto's SHA-256
// measured 4 to 7 times slower than this, and every token check hashes 4 to 6 blocks.
const rotr = std.math.rotr;
const SHA_K = [64]u32{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
fn shaBlock(h: *[8]u32, p: *const [64]u8) void {
    var w: [64]u32 = undefined;
    for (0..16) |i| w[i] = std.mem.readInt(u32, p[i * 4 ..][0..4], .big);
    for (16..64) |i| w[i] = w[i - 16] +% (rotr(u32, w[i - 15], 7) ^ rotr(u32, w[i - 15], 18) ^ (w[i - 15] >> 3)) +% w[i - 7] +% (rotr(u32, w[i - 2], 17) ^ rotr(u32, w[i - 2], 19) ^ (w[i - 2] >> 10));
    var v = h.*;
    for (0..64) |i| {
        const t1 = v[7] +% (rotr(u32, v[4], 6) ^ rotr(u32, v[4], 11) ^ rotr(u32, v[4], 25)) +% ((v[4] & v[5]) ^ (~v[4] & v[6])) +% SHA_K[i] +% w[i];
        const t2 = (rotr(u32, v[0], 2) ^ rotr(u32, v[0], 13) ^ rotr(u32, v[0], 22)) +% ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        const nv = [8]u32{ t1 +% t2, v[0], v[1], v[2], v[3] +% t1, v[4], v[5], v[6] }; // not in place: it reads v
        v = nv;
    }
    for (h, v) |*x, y| x.* +%= y;
}
fn shaFinish(h0: [8]u32, m: []const u8, out: *[32]u8) void { // hashes m after one 64-byte block already in h0
    var h = h0;
    var p = m;
    while (p.len >= 64) : (p = p[64..]) shaBlock(&h, p[0..64]);
    var b = [_]u8{0} ** 128;
    @memcpy(b[0..p.len], p);
    b[p.len] = 0x80;
    const n: usize = if (p.len < 56) 64 else 128;
    std.mem.writeInt(u64, b[n - 8 ..][0..8], (64 + m.len) * 8, .big);
    shaBlock(&h, b[0..64]);
    if (n == 128) shaBlock(&h, b[64..128]);
    for (h, 0..) |x, i| std.mem.writeInt(u32, out[i * 4 ..][0..4], x, .big);
}
var hm_in: [8]u32 = undefined; // HMAC key schedule: hash state after key^ipad / key^opad, computed once
var hm_out: [8]u32 = undefined;
fn hmacKey(key: []const u8) void {
    var k = [_]u8{0} ** 64;
    if (key.len > 64) std.crypto.hash.sha2.Sha256.hash(key, k[0..32], .{}) else @memcpy(k[0..key.len], key);
    const iv = [8]u32{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    var pad: [64]u8 = undefined;
    for (&pad, k) |*x, y| x.* = y ^ 0x36;
    hm_in = iv;
    shaBlock(&hm_in, &pad);
    for (&pad, k) |*x, y| x.* = y ^ 0x5c;
    hm_out = iv;
    shaBlock(&hm_out, &pad);
}
fn hmac(m: []const u8, out: *[32]u8) void {
    var ih: [32]u8 = undefined;
    shaFinish(hm_in, m, &ih);
    shaFinish(hm_out, &ih, out);
}
test "hmac matches std" {
    const H = std.crypto.auth.hmac.sha2.HmacSha256;
    var m: [300]u8 = undefined;
    for (&m, 0..) |*x, i| x.* = @truncate(i * 7);
    for ([_][]const u8{ "k", "twelve-dollar-challenge", "x" ** 64, "y" ** 100 }) |key| {
        hmacKey(key);
        for (0..m.len) |n| {
            var a: [32]u8 = undefined;
            var b: [32]u8 = undefined;
            hmac(m[0..n], &a);
            H.create(&b, m[0..n], key);
            try std.testing.expectEqualSlices(u8, &b, &a);
        }
    }
}

// ---------- base64url ----------
var b64t: [256]i8 = [_]i8{-1} ** 256;
fn b64dec(s: []const u8, out: []u8) isize { // -1 on bad input
    if (s.len % 4 == 1) return -1;
    var acc: u32 = 0;
    var bits: u5 = 0;
    var o: usize = 0;
    for (s) |ch| {
        const v = b64t[ch];
        if (v < 0) return -1;
        acc = (acc << 6) | @as(u32, @intCast(v));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o] = @truncate(acc >> bits);
            o += 1;
        }
    }
    return @intCast(o);
}

// ---------- JSON (strict, RFC 8259) ----------
const Js = struct { s: []const u8, p: usize };
fn ws(j: *Js) void {
    while (j.p < j.s.len) : (j.p += 1) switch (j.s[j.p]) {
        ' ', '\t', '\n', '\r' => {},
        else => return,
    };
}
fn hex4(b: []const u8) ?u32 {
    var v: u32 = 0;
    for (b[0..4]) |ch| {
        const d: u32 = switch (ch) {
            '0'...'9' => ch - '0',
            'a'...'f' => ch - 'a' + 10,
            'A'...'F' => ch - 'A' + 10,
            else => return null,
        };
        v = v << 4 | d;
    }
    return v;
}
fn utf8Len(s: []const u8, p: usize) usize { // length of one valid UTF-8 sequence at p, 0 if invalid
    const ch = s[p];
    const left = s.len - p;
    if (ch < 0x80) return 1;
    if (ch >= 0xc2 and ch <= 0xdf) return if (left >= 2 and s[p + 1] & 0xc0 == 0x80) 2 else 0;
    if (ch >= 0xe0 and ch <= 0xef) {
        if (left < 3 or s[p + 1] & 0xc0 != 0x80 or s[p + 2] & 0xc0 != 0x80) return 0;
        if ((ch == 0xe0 and s[p + 1] < 0xa0) or (ch == 0xed and s[p + 1] > 0x9f)) return 0;
        return 3;
    }
    if (ch >= 0xf0 and ch <= 0xf4) {
        if (left < 4 or s[p + 1] & 0xc0 != 0x80 or s[p + 2] & 0xc0 != 0x80 or s[p + 3] & 0xc0 != 0x80) return 0;
        if ((ch == 0xf0 and s[p + 1] < 0x90) or (ch == 0xf4 and s[p + 1] > 0x8f)) return 0;
        return 4;
    }
    return 0;
}
// Parses the string at j.p (on its opening quote), decoding UTF-8 into out when out is non-null.
// Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit (the string is still validated).
fn jstr(j: *Js, out_in: ?[]u8) isize {
    const s = j.s;
    var out = out_in;
    var over = false;
    var p = j.p + 1;
    var o: usize = 0;
    while (p < s.len) {
        const ch = s[p];
        if (ch == '"') {
            j.p = p + 1;
            return if (over) -2 else @intCast(o);
        }
        if (ch < 0x20) return -1;
        if (ch == '\\') {
            p += 1;
            if (p >= s.len) return -1;
            const e: u8 = switch (s[p]) {
                '"' => '"',
                '\\' => '\\',
                '/' => '/',
                'b' => 8,
                'f' => 12,
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                'u' => {
                    if (s.len - p < 5) return -1;
                    var cp = hex4(s[p + 1 ..]) orelse return -1;
                    p += 4;
                    if (cp >= 0xd800 and cp <= 0xdbff and s.len - p >= 7 and s[p + 1] == '\\' and s[p + 2] == 'u') {
                        const lo = hex4(s[p + 3 ..]) orelse return -1;
                        if (lo >= 0xdc00 and lo <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            p += 6;
                        }
                    }
                    if (cp >= 0xd800 and cp <= 0xdfff) cp = 0xfffd; // lone surrogate: U+FFFD, as JS TextEncoder does
                    var u: [4]u8 = undefined;
                    const ul: usize = std.unicode.utf8Encode(@intCast(cp), &u) catch unreachable;
                    if (out) |ob| {
                        if (o + ul > ob.len) {
                            out = null;
                            over = true;
                        } else @memcpy(ob[o..][0..ul], u[0..ul]);
                    }
                    o += ul;
                    p += 1;
                    continue;
                },
                else => return -1,
            };
            if (out) |ob| {
                if (o >= ob.len) {
                    out = null;
                    over = true;
                } else ob[o] = e;
            }
            o += 1;
            p += 1;
            continue;
        }
        const l = utf8Len(s, p);
        if (l == 0) return -1;
        if (out) |ob| {
            if (o + l > ob.len) {
                out = null;
                over = true;
            } else @memcpy(ob[o..][0..l], s[p..][0..l]);
        }
        o += l;
        p += l;
    }
    return -1;
}
fn isDigit(ch: u8) bool {
    return ch >= '0' and ch <= '9';
}
fn jnumber(j: *Js) bool {
    const s = j.s;
    var p = j.p;
    if (p < s.len and s[p] == '-') p += 1;
    if (p >= s.len) return false;
    if (s[p] == '0') {
        p += 1;
    } else if (s[p] >= '1' and s[p] <= '9') {
        while (p < s.len and isDigit(s[p])) p += 1;
    } else return false;
    if (p < s.len and s[p] == '.') {
        p += 1;
        if (p >= s.len or !isDigit(s[p])) return false;
        while (p < s.len and isDigit(s[p])) p += 1;
    }
    if (p < s.len and (s[p] == 'e' or s[p] == 'E')) {
        p += 1;
        if (p < s.len and (s[p] == '+' or s[p] == '-')) p += 1;
        if (p >= s.len or !isDigit(s[p])) return false;
        while (p < s.len and isDigit(s[p])) p += 1;
    }
    j.p = p;
    return true;
}
fn jlit(j: *Js, lit: []const u8) bool {
    if (!std.mem.startsWith(u8, j.s[j.p..], lit)) return false;
    j.p += lit.len;
    return true;
}
fn jvalue(j: *Js, depth: u32) bool { // validates and skips one value
    if (depth > 64) return false;
    ws(j);
    if (j.p >= j.s.len) return false;
    switch (j.s[j.p]) {
        '"' => return jstr(j, null) >= 0,
        '{' => {
            j.p += 1;
            ws(j);
            if (j.p < j.s.len and j.s[j.p] == '}') {
                j.p += 1;
                return true;
            }
            while (true) {
                ws(j);
                if (j.p >= j.s.len or j.s[j.p] != '"' or jstr(j, null) < 0) return false;
                ws(j);
                if (j.p >= j.s.len or j.s[j.p] != ':') return false;
                j.p += 1;
                if (!jvalue(j, depth + 1)) return false;
                ws(j);
                if (j.p >= j.s.len) return false;
                if (j.s[j.p] == ',') {
                    j.p += 1;
                    continue;
                }
                if (j.s[j.p] == '}') {
                    j.p += 1;
                    return true;
                }
                return false;
            }
        },
        '[' => {
            j.p += 1;
            ws(j);
            if (j.p < j.s.len and j.s[j.p] == ']') {
                j.p += 1;
                return true;
            }
            while (true) {
                if (!jvalue(j, depth + 1)) return false;
                ws(j);
                if (j.p >= j.s.len) return false;
                if (j.s[j.p] == ',') {
                    j.p += 1;
                    continue;
                }
                if (j.s[j.p] == ']') {
                    j.p += 1;
                    return true;
                }
                return false;
            }
        },
        't' => return jlit(j, "true"),
        'f' => return jlit(j, "false"),
        'n' => return jlit(j, "null"),
        else => return jnumber(j),
    }
}
// Validates a whole document. For a top-level object it records the raw span of the last value of
// each wanted key (the last duplicate wins, as in JSON.parse). Returns false if the document is invalid.
const Want = struct { key: []const u8, found: bool = false, v: usize = 0, ve: usize = 0 };
fn jdoc(s: []const u8, w: []Want) bool {
    var j = Js{ .s = s, .p = 0 };
    for (w) |*x| x.found = false;
    ws(&j);
    if (j.p < s.len and s[j.p] == '{') {
        j.p += 1;
        ws(&j);
        if (j.p < s.len and s[j.p] == '}') {
            j.p += 1;
        } else while (true) {
            var kb: [32]u8 = undefined;
            ws(&j);
            if (j.p >= s.len or s[j.p] != '"') return false;
            const kl = jstr(&j, &kb);
            if (kl == -1) return false;
            ws(&j);
            if (j.p >= s.len or s[j.p] != ':') return false;
            j.p += 1;
            ws(&j);
            const vs = j.p;
            if (!jvalue(&j, 1)) return false;
            if (kl >= 0) for (w) |*x| {
                if (std.mem.eql(u8, kb[0..@intCast(kl)], x.key)) {
                    x.found = true;
                    x.v = vs;
                    x.ve = j.p;
                }
            };
            ws(&j);
            if (j.p >= s.len) return false;
            if (s[j.p] == ',') {
                j.p += 1;
                continue;
            }
            if (s[j.p] == '}') {
                j.p += 1;
                break;
            }
            return false;
        }
    } else if (!jvalue(&j, 0)) return false;
    ws(&j);
    return j.p == s.len;
}

// ---------- auth ----------
const Auth = struct { uid: i64 = 0, uname: [1024]u8 = undefined, unl: usize = 0 };
// Returns null and fills a on success, else the message to send with the 401.
fn auth(h: ?[]const u8, a: *Auth) ?[]const u8 {
    const bad = "invalid or expired token";
    const payload = "invalid token payload";
    const hv = h orelse return "missing bearer token";
    if (!std.mem.startsWith(u8, hv, "Bearer ")) return "missing bearer token";
    const t = hv[7..];
    const d1 = std.mem.indexOfScalar(u8, t, '.') orelse return bad;
    const d2 = std.mem.indexOfScalarPos(u8, t, d1 + 1, '.') orelse return bad;
    if (std.mem.indexOfScalarPos(u8, t, d2 + 1, '.') != null) return bad;
    var sig: [48]u8 = undefined;
    var mac: [32]u8 = undefined;
    var hdr: [512]u8 = undefined;
    var pl: [2048]u8 = undefined;
    if (t.len - d2 - 1 != 43 or b64dec(t[d2 + 1 ..], &sig) != 32) return bad;
    hmac(t[0..d2], &mac);
    var diff: u8 = 0;
    for (0..32) |i| diff |= sig[i] ^ mac[i];
    if (diff != 0) return bad;
    if (d1 > 680 or d2 - d1 - 1 > 2700) return bad;
    const hn = b64dec(t[0..d1], &hdr);
    const pn = b64dec(t[d1 + 1 .. d2], &pl);
    if (hn < 0 or pn < 0) return bad;
    const hs = hdr[0..@intCast(hn)];
    var hw = [_]Want{.{ .key = "alg" }};
    if (!jdoc(hs, &hw) or !hw[0].found or hs[hw[0].v] != '"') return bad;
    var alg: [8]u8 = undefined;
    var aj = Js{ .s = hs, .p = hw[0].v };
    const al = jstr(&aj, &alg);
    if (al != 5 or !std.mem.eql(u8, alg[0..5], "HS256")) return bad;
    const ps = pl[0..@intCast(pn)];
    var pw = [_]Want{ .{ .key = "exp" }, .{ .key = "nbf" }, .{ .key = "sub" }, .{ .key = "username" } };
    if (!jdoc(ps, &pw) or !pw[0].found) return bad; // exp is required
    for (0..2) |i| if (pw[i].found) { // exp / nbf must be numbers, checked against now
        const span = ps[pw[i].v..pw[i].ve];
        if (span[0] != '-' and !isDigit(span[0])) return bad;
        const v = std.fmt.parseFloat(f64, span) catch return bad;
        const now: f64 = @floatFromInt(now_s);
        if (if (i == 0) now >= v else now < v) return bad;
    };
    if (!pw[2].found or ps[pw[2].v] != '"' or !pw[3].found or ps[pw[3].v] != '"') return payload;
    var sb: [24]u8 = undefined;
    var sj = Js{ .s = ps, .p = pw[2].v };
    const sl = jstr(&sj, &sb);
    if (sl <= 0 or sl > 15) return payload;
    var v: i64 = 0;
    for (sb[0..@intCast(sl)]) |d| {
        if (!isDigit(d)) return payload;
        v = v * 10 + (d - '0');
    }
    if (v <= 0) return payload;
    var uj = Js{ .s = ps, .p = pw[3].v };
    const ul = jstr(&uj, &a.uname);
    if (ul < 0) return payload;
    a.uid = v;
    a.unl = @intCast(ul);
    return null;
}

// ---------- database ----------
fn step(s: ?*c.sqlite3_stmt) c_int {
    return c.sqlite3_step(s);
}
fn reset(s: ?*c.sqlite3_stmt) void {
    _ = c.sqlite3_reset(s);
}
fn txn() bool { // opens the batch transaction on first use
    if (in_txn) return true;
    const rc = step(st_begin);
    reset(st_begin);
    in_txn = rc == c.SQLITE_DONE;
    return in_txn;
}
// Runs a write inside the batch transaction. With several processes, a read snapshot can't be upgraded
// once another process has committed (SQLITE_BUSY_SNAPSHOT): end the read-only part of the batch, take
// the write lock with BEGIN IMMEDIATE (which waits up to busy_timeout) and retry once.
fn wstep(s: ?*c.sqlite3_stmt) c_int {
    var rc = step(s);
    if (rc & 0xff == c.SQLITE_BUSY and !txn_writes) {
        reset(s);
        if (in_txn) {
            _ = step(st_commit);
            reset(st_commit);
            in_txn = false;
        }
        rc = step(st_beginimm);
        reset(st_beginimm);
        if (rc != c.SQLITE_DONE) return rc;
        in_txn = true;
        rc = step(s);
    }
    if (rc == c.SQLITE_ROW or rc == c.SQLITE_DONE) txn_writes = true;
    return rc;
}
fn dbFail() void { // a failed statement may have rolled the whole batch back
    if (in_txn and c.sqlite3_get_autocommit(db) != 0) {
        in_txn = false;
        if (txn_writes) batch_failed = true;
        txn_writes = false;
    }
}
fn col(s: ?*c.sqlite3_stmt, i: c_int) []const u8 {
    const p = c.sqlite3_column_text(s, i);
    if (p == null) return "";
    return p[0..@intCast(c.sqlite3_column_bytes(s, i))];
}
fn putPostRow(s: ?*c.sqlite3_stmt, likes: i64) void { // likes < 0: take column 4
    put("{\"id\":");
    putu(@intCast(c.sqlite3_column_int64(s, 0)));
    put(",\"body\":\"");
    putjs(col(s, 1));
    put("\",\"created_at\":\"");
    put(col(s, 2));
    put("\",\"author\":\"");
    putjs(col(s, 3));
    put("\",\"like_count\":");
    putu(@intCast(if (likes < 0) c.sqlite3_column_int64(s, 4) else likes));
    put("}");
}

// The feed's newest 20 ids usually sit in a narrow id range. Then one scan of that range in likes and
// one in posts replace the 20 per-row count and join lookups. Returns 1 when the feed was written,
// 0 to fall back to the reference query (ids too spread out), -1 on a database error.
var ftmp: [BATCH_OUT]u8 = undefined;
fn feedRange(close: bool) i32 {
    var ids: [20]i64 = undefined;
    var n: usize = 0;
    var rc = step(st_fids);
    while (rc == c.SQLITE_ROW) : (rc = step(st_fids)) {
        ids[n] = c.sqlite3_column_int64(st_fids, 0);
        n += 1;
    }
    reset(st_fids);
    if (rc != c.SQLITE_DONE) return -1;
    const lo = if (n > 0) std.mem.min(i64, ids[0..n]) else 0;
    const hi = if (n > 0) std.mem.max(i64, ids[0..n]) else 0;
    if (hi - lo >= 256) return 0;
    var slot = [_]i8{-1} ** 256;
    for (ids[0..n], 0..) |id, i| slot[@intCast(id - lo)] = @intCast(i);
    var likes = [_]i64{0} ** 20;
    _ = c.sqlite3_bind_int64(st_flikes, 1, lo);
    _ = c.sqlite3_bind_int64(st_flikes, 2, hi);
    rc = step(st_flikes);
    while (rc == c.SQLITE_ROW) : (rc = step(st_flikes)) {
        const k = slot[@intCast(c.sqlite3_column_int64(st_flikes, 0) - lo)];
        if (k >= 0) likes[@intCast(k)] += 1;
    }
    reset(st_flikes);
    if (rc != c.SQLITE_DONE) return -1;
    const mark = alen;
    var at: [20]usize = undefined;
    var len: [20]usize = undefined;
    var found: usize = 0;
    beginResp(200, close);
    put("{\"posts\":[");
    const rows = alen;
    _ = c.sqlite3_bind_int64(st_frows, 1, lo);
    _ = c.sqlite3_bind_int64(st_frows, 2, hi);
    rc = step(st_frows);
    while (rc == c.SQLITE_ROW) : (rc = step(st_frows)) {
        const k = slot[@intCast(c.sqlite3_column_int64(st_frows, 0) - lo)];
        if (k < 0) continue;
        const ki: usize = @intCast(k);
        at[ki] = alen;
        putPostRow(st_frows, likes[ki]);
        len[ki] = alen - at[ki];
        found += 1;
    }
    reset(st_frows);
    if (rc != c.SQLITE_DONE or found != n) {
        alen = mark;
        return if (rc != c.SQLITE_DONE) -1 else 0;
    }
    @memcpy(ftmp[0 .. alen - rows], arena[rows..alen]); // back into feed order
    alen = rows;
    for (0..n) |i| {
        if (i > 0) put(",");
        put(ftmp[at[i] - rows ..][0..len[i]]);
    }
    put("]}");
    finish();
    return 1;
}

// ---------- request handling ----------
fn parseId(s: []const u8) i64 { // positive decimal integer, else -1 (huge ids map to max: not found)
    if (s.len == 0) return -1;
    var v: i64 = 0;
    for (s) |d| {
        if (!isDigit(d)) return -1;
        v = if (v < 100000000000000000) v * 10 + (d - '0') else std.math.maxInt(i64);
    }
    return if (v > 0) v else -1;
}
fn jswsAt(s: []const u8, p: usize, e: usize) usize { // byte length of JS String.trim whitespace at p, or 0
    const ch = s[p];
    if (ch == ' ' or (ch >= 9 and ch <= 13)) return 1;
    if (e - p >= 2 and ch == 0xc2 and s[p + 1] == 0xa0) return 2;
    if (e - p >= 3) {
        const u = @as(u32, ch) << 16 | @as(u32, s[p + 1]) << 8 | s[p + 2];
        if (u == 0xe19a80 or (u >= 0xe28080 and u <= 0xe2808a) or u == 0xe280a8 or u == 0xe280a9 or u == 0xe280af or u == 0xe2819f or u == 0xe38080 or u == 0xefbbbf) return 3;
    }
    return 0;
}
fn jswsBefore(s: []const u8, b: usize, e: usize) usize {
    const ch = s[e - 1];
    if (ch == ' ' or (ch >= 9 and ch <= 13)) return 1;
    if (e - b >= 2 and s[e - 2] == 0xc2 and ch == 0xa0) return 2;
    if (e - b >= 3 and jswsAt(s, e - 3, e) == 3) return 3;
    return 0;
}

var txt: [MAXREQ]u8 = undefined;
fn handle(m: []const u8, path0: []const u8, authh: ?[]const u8, body: []const u8, close: bool) void {
    var path = path0;
    if (std.mem.indexOfScalar(u8, path, '?')) |q| path = path[0..q];
    const get = std.mem.eql(u8, m, "GET");
    const post = std.mem.eql(u8, m, "POST");
    if (get and std.mem.eql(u8, path, "/feed")) {
        if (!txn()) return err(500, "internal server error", close);
        const fr = feedRange(close);
        if (fr < 0) {
            dbFail();
            return err(500, "internal server error", close);
        }
        if (fr > 0) return;
        const mark = alen;
        beginResp(200, close);
        put("{\"posts\":[");
        var i: usize = 0;
        var rc = step(st_feed);
        while (rc == c.SQLITE_ROW) : (rc = step(st_feed)) {
            if (i > 0) put(",");
            i += 1;
            putPostRow(st_feed, -1);
        }
        reset(st_feed);
        if (rc != c.SQLITE_DONE) {
            dbFail();
            alen = mark;
            return err(500, "internal server error", close);
        }
        put("]}");
        return finish();
    }
    if (path.len > 7 and std.mem.startsWith(u8, path, "/posts/")) {
        const seg = path[7..];
        const sl = std.mem.indexOfScalar(u8, seg, '/');
        if (get and sl == null) {
            const id = parseId(seg);
            if (id < 0) return err(400, "invalid post id", close);
            if (!txn()) return err(500, "internal server error", close);
            _ = c.sqlite3_bind_int64(st_post, 1, id);
            const mark = alen;
            const rc = step(st_post);
            if (rc == c.SQLITE_ROW) {
                beginResp(200, close);
                put("{\"post\":");
                putPostRow(st_post, -1);
                put("}");
                finish();
            } else if (rc == c.SQLITE_DONE) {
                err(404, "post not found", close);
            } else {
                dbFail();
                alen = mark;
                err(500, "internal server error", close);
            }
            return reset(st_post);
        }
        if (post and sl != null and std.mem.eql(u8, seg[sl.?..], "/like")) {
            var a: Auth = .{};
            if (auth(authh, &a)) |e| return err(401, e, close);
            const id = parseId(seg[0..sl.?]);
            if (id < 0) return err(400, "invalid post id", close);
            if (!txn()) return err(500, "internal server error", close);
            _ = c.sqlite3_bind_int64(st_like, 1, a.uid);
            _ = c.sqlite3_bind_int64(st_like, 2, id);
            var rc = wstep(st_like);
            reset(st_like);
            if (rc != c.SQLITE_DONE) {
                dbFail();
                return err(500, "internal server error", close);
            }
            const created = c.sqlite3_changes(db) > 0;
            if (!created) {
                _ = c.sqlite3_bind_int64(st_exists, 1, id);
                rc = step(st_exists);
                reset(st_exists);
                if (rc == c.SQLITE_DONE) return err(404, "post not found", close);
                if (rc != c.SQLITE_ROW) {
                    dbFail();
                    return err(500, "internal server error", close);
                }
            }
            beginResp(if (created) 201 else 200, close);
            put("{\"liked\":true,\"already_liked\":");
            put(if (created) "false" else "true");
            put(",\"post_id\":");
            putu(@intCast(id));
            put("}");
            return finish();
        }
    }
    if (post and std.mem.eql(u8, path, "/posts")) {
        var a: Auth = .{};
        if (auth(authh, &a)) |e| return err(401, e, close);
        var w = [_]Want{.{ .key = "body" }};
        if (!jdoc(body, &w)) return err(400, "malformed JSON body", close);
        if (!w[0].found or body[w[0].v] != '"') return err(400, "body is required", close);
        var bj = Js{ .s = body, .p = w[0].v };
        const n = jstr(&bj, &txt);
        if (n < 0) return err(400, "malformed JSON body", close);
        var s: usize = 0;
        var se: usize = @intCast(n);
        while (s < se) {
            const l = jswsAt(&txt, s, se);
            if (l == 0) break;
            s += l;
        }
        while (se > s) {
            const l = jswsBefore(&txt, s, se);
            if (l == 0) break;
            se -= l;
        }
        if (s == se) return err(400, "body is required", close);
        var chars: usize = 0;
        for (txt[s..se]) |ch| chars += @intFromBool(ch & 0xc0 != 0x80);
        if (chars > 500) return err(400, "body must be at most 500 characters", close);
        if (!txn()) return err(500, "internal server error", close);
        _ = c.sqlite3_bind_int64(st_ins, 1, a.uid);
        _ = c.sqlite3_bind_text(st_ins, 2, &txt[s], @intCast(se - s), null);
        var rc = wstep(st_ins);
        if (rc != c.SQLITE_ROW) {
            reset(st_ins);
            dbFail();
            return err(500, "internal server error", close);
        }
        const mark = alen;
        beginResp(201, close);
        put("{\"post\":{\"id\":");
        putu(@intCast(c.sqlite3_column_int64(st_ins, 0)));
        put(",\"body\":\"");
        putjs(txt[s..se]);
        put("\",\"created_at\":\"");
        put(col(st_ins, 1));
        put("\",\"author\":\"");
        putjs(a.uname[0..a.unl]);
        put("\",\"like_count\":0}}");
        finish();
        rc = step(st_ins); // run RETURNING to completion so the insert finishes
        reset(st_ins);
        if (rc != c.SQLITE_DONE) {
            dbFail();
            alen = mark;
            err(500, "internal server error", close);
        }
        return;
    }
    if (get and std.mem.eql(u8, path, "/health")) {
        const rc = step(st_one);
        reset(st_one);
        if (rc == c.SQLITE_ROW) {
            beginResp(200, close);
            put("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            putu(@intCast(now_s - started));
            put("}");
        } else {
            beginResp(503, close);
            put("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
            putjs(std.mem.span(c.sqlite3_errmsg(db)));
            put("\"}");
        }
        return finish();
    }
    err(404, "not found", close);
}

// Serves every complete request in buf. Returns bytes consumed, or -1 to drop the connection.
fn serve(fd: i32, buf: []u8, closing: *bool) isize {
    const eq = std.ascii.eqlIgnoreCase;
    var used: usize = 0;
    while (used < buf.len and !closing.*) {
        if (alen + 2 * MAXREQ > BATCH_OUT) { // the rest waits for the next batch
            arena_full = true;
            break;
        }
        const r = buf[used..];
        const he = std.mem.indexOf(u8, r, "\r\n\r\n") orelse {
            if (r.len > MAXREQ) return -1;
            break;
        };
        const hdrs = r[0 .. he + 2];
        const le = std.mem.indexOfScalar(u8, hdrs, '\r').?;
        const line = r[0..le];
        const sp1 = std.mem.indexOfScalar(u8, line, ' ') orelse return -1;
        const sp2 = std.mem.indexOfScalarPos(u8, line, sp1 + 1, ' ') orelse return -1;
        var close = std.mem.eql(u8, line[sp2 + 1 ..], "HTTP/1.0");
        var authh: ?[]const u8 = null;
        var clen: usize = 0;
        var expect = false;
        var h = le + 2;
        while (h < hdrs.len) {
            const hl = std.mem.indexOfScalarPos(u8, hdrs, h, '\r').?;
            const hline = r[h..hl];
            h = hl + 2;
            const ci = std.mem.indexOfScalar(u8, hline, ':') orelse continue;
            const name = hline[0..ci];
            const v = std.mem.trim(u8, hline[ci + 1 ..], " \t");
            if (eq(name, "content-length")) {
                clen = 0;
                for (v) |d| {
                    if (!isDigit(d)) return -1;
                    clen = clen * 10 + (d - '0');
                    if (clen > MAXREQ) return -1;
                }
            } else if (eq(name, "authorization")) {
                authh = v;
            } else if (eq(name, "connection")) {
                if (eq(v, "close")) close = true else if (eq(v, "keep-alive")) close = false;
            } else if (eq(name, "expect")) {
                expect = eq(v, "100-continue");
            } else if (eq(name, "transfer-encoding")) return -1; // chunked uploads aren't supported
        }
        const body = he + 4;
        if (r.len - body < clen) {
            if (expect and body == r.len) {
                const cont = "HTTP/1.1 100 Continue\r\n\r\n";
                if (failed(linux.write(fd, cont, cont.len))) return -1;
            }
            break;
        }
        handle(line[0..sp1], line[sp1 + 1 .. sp2], authh, r[body .. body + clen], close);
        if (close) closing.* = true;
        used += body + clen;
    }
    return @intCast(used);
}

// ---------- connections ----------
fn connClose(fd: i32) void {
    const cn = &conns[@intCast(fd)];
    if (cn.in.len > 0) gpa.free(cn.in);
    if (cn.out.len > 0) gpa.free(cn.out);
    cn.* = .{};
    _ = linux.close(fd);
    nconns -= 1;
}
fn watch(fd: i32, ev: u32) void {
    var e = linux.epoll_event{ .events = ev, .data = .{ .fd = fd } };
    _ = linux.epoll_ctl(epfd, linux.EPOLL.CTL_MOD, fd, &e);
}

fn process(fd: i32, buf: []u8) void {
    const cn = &conns[@intCast(fd)];
    const start = alen;
    var closing = false;
    arena_full = false;
    const ui = serve(fd, buf, &closing);
    if (ui < 0) {
        alen = start;
        return connClose(fd);
    }
    const used: usize = @intCast(ui);
    if (alen > start) {
        segs[nsegs] = .{ .fd = fd, .off = start, .len = alen - start };
        nsegs += 1;
    }
    cn.closing = closing;
    const left = if (closing) 0 else buf.len - used;
    if (left > 0) {
        if (buf.ptr != cn.in.ptr) {
            cn.in = gpa.alloc(u8, left + RBUF) catch return connClose(fd);
            @memcpy(cn.in[0..left], buf[used..]);
        } else if (used > 0) {
            std.mem.copyForwards(u8, cn.in[0..left], cn.in[used .. used + left]);
        }
        cn.inlen = left;
        if (arena_full) {
            deferred[ndef] = fd;
            ndef += 1;
        }
    } else if (cn.in.len > 0) {
        gpa.free(cn.in);
        cn.in = &.{};
        cn.inlen = 0;
    }
    if (closing and alen == start) connClose(fd);
}

var rbuf: [RBUF]u8 = undefined;
fn onReadable(fd: i32) void {
    const cn = &conns[@intCast(fd)];
    if (cn.out.len > 0) return; // still sending an earlier response; reading resumes once it is out
    if (cn.inlen > 0) {
        if (cn.in.len - cn.inlen < 4096) cn.in = gpa.realloc(cn.in, cn.in.len * 2 + RBUF) catch return connClose(fd);
        const r = linux.read(fd, cn.in[cn.inlen..].ptr, cn.in.len - cn.inlen);
        if (failed(r) or r == 0) {
            if (failed(r) and errno(r) == EAGAIN) return;
            return connClose(fd);
        }
        cn.inlen += r;
        process(fd, cn.in[0..cn.inlen]);
    } else {
        const r = linux.read(fd, &rbuf, RBUF);
        if (failed(r) or r == 0) {
            if (failed(r) and errno(r) == EAGAIN) return;
            return connClose(fd);
        }
        process(fd, rbuf[0..r]);
    }
}
fn onWritable(fd: i32) void {
    const cn = &conns[@intCast(fd)];
    if (cn.out.len == 0) return;
    const w = linux.write(fd, cn.out[cn.outoff..].ptr, cn.out.len - cn.outoff);
    if (failed(w)) {
        if (errno(w) == EAGAIN) return;
        return connClose(fd);
    }
    cn.outoff += w;
    if (cn.outoff < cn.out.len) return;
    gpa.free(cn.out);
    cn.out = &.{};
    cn.outoff = 0;
    if (cn.closing) return connClose(fd);
    watch(fd, linux.EPOLL.IN);
    if (cn.inlen > 0) process(fd, cn.in[0..cn.inlen]);
}

// Commits the batch, then sends every response produced in it.
fn flushBatch() void {
    if (in_txn) {
        const rc = step(st_commit);
        reset(st_commit);
        if (rc != c.SQLITE_DONE) {
            _ = step(st_rollback);
            reset(st_rollback);
            if (txn_writes) batch_failed = true;
        }
        in_txn = false;
        txn_writes = false;
    }
    for (segs[0..nsegs]) |s| {
        const cn = &conns[@intCast(s.fd)];
        if (!cn.open) continue;
        if (batch_failed) { // the batch wasn't committed: never claim it was
            connClose(s.fd);
            continue;
        }
        var w = linux.write(s.fd, arena[s.off..].ptr, s.len);
        if (failed(w)) {
            if (errno(w) != EAGAIN) {
                connClose(s.fd);
                continue;
            }
            w = 0;
        }
        if (w < s.len) {
            cn.out = gpa.alloc(u8, s.len - w) catch {
                connClose(s.fd);
                continue;
            };
            @memcpy(cn.out, arena[s.off + w .. s.off + s.len]);
            cn.outoff = 0;
            watch(s.fd, linux.EPOLL.OUT);
        } else if (cn.closing) connClose(s.fd);
    }
    nsegs = 0;
    alen = 0;
    batch_failed = false;
}

fn prep(sql: [:0]const u8) ?*c.sqlite3_stmt {
    var s: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v3(db, sql.ptr, -1, c.SQLITE_PREPARE_PERSISTENT, &s, null) != c.SQLITE_OK) {
        std.debug.print("prepare: {s}: {s}\n", .{ c.sqlite3_errmsg(db), sql });
        std.process.exit(1);
    }
    return s;
}
fn dbOpen(path: [:0]const u8) void {
    if (c.sqlite3_open_v2(path.ptr, &db, c.SQLITE_OPEN_READWRITE, null) != c.SQLITE_OK) {
        std.debug.print("open {s}: {s}\n", .{ path, c.sqlite3_errmsg(db) });
        std.process.exit(1);
    }
    _ = c.sqlite3_busy_timeout(db, 5000);
    const pragmas = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000;" ++
        "SELECT count(*) FROM users WHERE id = 1;";
    if (c.sqlite3_exec(db, pragmas, null, null, null) != c.SQLITE_OK) {
        std.debug.print("pragma: {s}\n", .{c.sqlite3_errmsg(db)});
        std.process.exit(1);
    }
    // The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
    // for a long time), so the WAL stays bounded at about 256 MB.
    _ = c.sqlite3_exec(db, "PRAGMA wal_autocheckpoint=65536;", null, null, null);
    const cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
    st_feed = prep(cols ++ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(cols ++ "WHERE p.id = ?");
    st_fids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    st_flikes = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?");
    st_frows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?");
    st_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    st_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
    st_exists = prep("SELECT 1 FROM posts WHERE id = ?");
    st_one = prep("SELECT 1");
    st_begin = prep("BEGIN");
    st_beginimm = prep("BEGIN IMMEDIATE");
    st_commit = prep("COMMIT");
    st_rollback = prep("ROLLBACK");
    _ = step(st_feed); // takes the lock and touches the hot pages before the first request
    reset(st_feed);
}

var g_host: []const u8 = "";
var g_port: u16 = 0;
var lfd: i32 = -1;
var conn_hi: usize = 0;
var conn_lo: usize = 0;
var conn_max: usize = 0;
var spawn_pending = false;
var spawned = false;
var ready_fd: i32 = -1;

fn epollAdd(fd: i32) void {
    var e = linux.epoll_event{ .events = linux.EPOLL.IN, .data = .{ .fd = fd } };
    _ = linux.epoll_ctl(epfd, linux.EPOLL.CTL_ADD, fd, &e);
}
fn listenStart() void {
    const addr = std.net.Address.parseIp4(g_host, g_port) catch {
        std.debug.print("bad HOST {s}\n", .{g_host});
        std.process.exit(1);
    };
    const fd: i32 = @intCast(linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC, 0));
    const one = std.mem.toBytes(@as(c_int, 1));
    _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEADDR, &one, one.len);
    _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEPORT, &one, one.len);
    _ = linux.setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, &one, one.len);
    if (failed(linux.bind(fd, &addr.any, addr.getOsSockLen())) or failed(linux.listen(fd, 65535))) {
        std.debug.print("bind/listen {s}:{d} failed\n", .{ g_host, g_port });
        std.process.exit(1);
    }
    epollAdd(fd);
    lfd = fd;
}

// Every keep-alive client holds a descriptor, and LimitNOFILE caps descriptors per process. Close to
// that cap this process starts one more copy of itself on the same port (SO_REUSEPORT) and stops
// accepting until it has room again, so new clients go to the other copy. Below the cap there is one process.
fn spawnWorker() void {
    spawn_pending = false;
    spawned = true;
    var pfd: [2]c_int = undefined;
    if (pipe2(&pfd, O_CLOEXEC) != 0) return;
    const p = fork();
    if (p == 0) {
        _ = linux.prctl(@intFromEnum(linux.PR.SET_PDEATHSIG), 15, 0, 0, 0); // SIGTERM if the parent dies
        _ = linux.fcntl(pfd[1], linux.F.SETFD, 0);
        var fdarg: [16:0]u8 = undefined;
        const a = std.fmt.bufPrintZ(&fdarg, "{d}", .{pfd[1]}) catch unreachable;
        const argv = [_:null]?[*:0]const u8{ "server", "--shared", a.ptr };
        _ = execv("/proc/self/exe", &argv);
        linux.exit(1);
    }
    _ = linux.close(pfd[1]);
    ready_fd = pfd[0];
    epollAdd(ready_fd);
    std.debug.print("{d} connections: starting worker {d}\n", .{ nconns, p });
}
fn acceptConns(limit: usize) void {
    while (nconns < limit) {
        const a = linux.accept4(lfd, null, null, linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC);
        if (failed(a)) break;
        const cfd: i32 = @intCast(a);
        if (cfd >= MAXFD) {
            _ = linux.close(cfd);
            continue;
        }
        conns[@intCast(cfd)].open = true;
        nconns += 1;
        epollAdd(cfd);
    }
}
fn listenStop() void { // take what is already queued, then leave new clients to the other worker
    acceptConns(conn_max);
    _ = linux.close(lfd);
    lfd = -1;
}

// Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
// the length of the fsync, so a helper process does them and the event loop never waits on the disk.
fn checkpointer(path: [:0]const u8) noreturn {
    _ = linux.prctl(@intFromEnum(linux.PR.SET_PDEATHSIG), 15, 0, 0, 0);
    _ = c.sqlite3_initialize();
    if (c.sqlite3_open_v2(path.ptr, &db, c.SQLITE_OPEN_READWRITE, null) != c.SQLITE_OK) std.process.exit(1);
    _ = c.sqlite3_busy_timeout(db, 100);
    _ = c.sqlite3_exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;", null, null, null);
    while (true) {
        var log: c_int = 0;
        var done: c_int = 0;
        _ = c.sqlite3_wal_checkpoint_v2(db, null, c.SQLITE_CHECKPOINT_PASSIVE, &log, &done);
        std.Thread.sleep(if (log > 0 and done < log) std.time.ns_per_ms else 10 * std.time.ns_per_ms);
    }
}

pub fn main() !void {
    const path = std.posix.getenv("SQLITE_PATH") orelse return error.MissingSqlitePath;
    const secret = std.posix.getenv("JWT_SECRET") orelse return error.MissingJwtSecret;
    g_host = std.posix.getenv("HOST") orelse "0.0.0.0";
    g_port = if (std.posix.getenv("PORT")) |p| try std.fmt.parseInt(u16, p, 10) else 3000;
    const args = std.os.argv;
    if (args.len > 1 and std.mem.eql(u8, std.mem.span(args[1]), "--ckpt")) checkpointer(path);
    const notify_fd: i32 = if (args.len > 2) try std.fmt.parseInt(i32, std.mem.span(args[2]), 10) else -1;
    _ = signal(13, 1); // SIGPIPE: SIG_IGN
    _ = signal(17, 1); // SIGCHLD: SIG_IGN, workers are reaped automatically
    var lim: usize = MAXFD;
    if (std.posix.getrlimit(.NOFILE)) |rl0| {
        var rl = rl0;
        if (rl.cur < rl.max) {
            rl.cur = rl.max;
            std.posix.setrlimit(.NOFILE, rl) catch {};
        }
        if (rl.cur < lim) lim = @intCast(rl.cur);
    } else |_| {}
    conn_hi = lim - @max(lim / 16, 64); // leave room for SQLite's files and the listener
    conn_lo = conn_hi - conn_hi / 8;
    conn_max = lim - 16;
    for ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 0..) |ch, i| b64t[ch] = @intCast(i);
    hmacKey(secret);
    _ = c.sqlite3_initialize(); // built with SQLITE_OMIT_AUTOINIT
    started = std.time.timestamp();
    now_s = started;

    conns = try gpa.alloc(Conn, MAXFD);
    @memset(conns, .{});
    segs = try gpa.alloc(Seg, MAXFD);
    deferred = try gpa.alloc(i32, MAXFD);
    const todo = try gpa.alloc(i32, MAXFD);
    arena = try gpa.alloc(u8, BATCH_OUT);

    dbOpen(path);
    if (args.len == 1 and fork() == 0) {
        const argv = [_:null]?[*:0]const u8{ "server", "--ckpt" };
        _ = execv("/proc/self/exe", &argv);
        linux.exit(1);
    }
    epfd = @intCast(linux.epoll_create1(linux.EPOLL.CLOEXEC));
    listenStart();
    if (notify_fd >= 0) { // tell the parent worker we're listening
        _ = linux.write(notify_fd, "1", 1);
        _ = linux.close(notify_fd);
    }
    var evs: [512]linux.epoll_event = undefined;
    while (true) {
        const rc = linux.epoll_wait(epfd, &evs, evs.len, -1);
        if (failed(rc)) {
            if (errno(rc) == EINTR) continue;
            return error.EpollWait;
        }
        now_s = std.time.timestamp();
        for (evs[0..rc]) |e| {
            const fd = e.data.fd;
            if (fd == lfd) {
                acceptConns(conn_hi);
                if (nconns >= conn_hi) { // nearly out of descriptors: new clients go to another worker
                    if (spawned) {
                        listenStop();
                    } else {
                        _ = linux.epoll_ctl(epfd, linux.EPOLL.CTL_DEL, lfd, null);
                        spawn_pending = true;
                    }
                }
                continue;
            }
            if (fd == ready_fd) { // the new worker is listening
                var b: [1]u8 = undefined;
                const up = linux.read(ready_fd, &b, 1) == 1;
                _ = linux.close(ready_fd);
                ready_fd = -1;
                if (up) {
                    listenStop();
                } else { // it failed to start
                    spawned = false;
                    epollAdd(lfd);
                }
                continue;
            }
            if (!conns[@intCast(fd)].open) continue;
            if (e.events & linux.EPOLL.OUT != 0) {
                onWritable(fd);
            } else if (e.events & (linux.EPOLL.IN | linux.EPOLL.HUP | linux.EPOLL.ERR) != 0) onReadable(fd);
        }
        flushBatch();
        while (ndef > 0) { // requests held back by a full arena
            const k = ndef;
            ndef = 0;
            @memcpy(todo[0..k], deferred[0..k]);
            for (todo[0..k]) |fd| {
                const cn = &conns[@intCast(fd)];
                if (cn.open and cn.inlen > 0 and cn.out.len == 0) process(fd, cn.in[0..cn.inlen]);
            }
            flushBatch();
        }
        if (spawn_pending) spawnWorker();
        if (lfd < 0 and nconns < conn_lo) listenStart();
    }
}
