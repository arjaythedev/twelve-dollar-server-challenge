// Feed API: one io_uring event loop, SQLite in-process, group commit per completion batch.
// A Zig port of our C entry (c-iouring-unorig-username); see README.md. Config comes only from SQLITE_PATH,
// JWT_SECRET, HOST, PORT. The only C is src/btree.c: SQLite's amalgamation plus wrappers for its B-tree calls.
const std = @import("std");
const linux = std.os.linux;
pub const c = @cImport(@cInclude("sqlite3.h"));

// The two changes of our C entry's best local build (4298dd7): one pwrite per WAL commit and a 2,048-buffer
// send pool. false gives the rig-measured C design (cae940e) exactly.
const COMBO = true;

const MAXREQ = 64 * 1024; // header + body cap per request
const BATCH_OUT = 4 << 20; // response bytes held per batch until its commit
const MAXSEG = 65536; // responses per batch
const NBUF = 2048; // provided receive buffers per ring: every one gets touched, so they cost RAM
const BSZ = 1024;
const CACHE_KB = 32768; // SQLite page cache
const MAXRINGS = 32;
const NLISTEN = 8; // SO_REUSEPORT listeners: each has its own accept queue of somaxconn

extern "c" fn memmem(h: [*]const u8, hl: usize, n: [*]const u8, nl: usize) ?[*]u8;
extern "c" fn strtod(s: [*:0]const u8, end: ?*[*]u8) f64;
extern "c" fn malloc(n: usize) ?*anyopaque;
extern "c" fn realloc(p: ?*anyopaque, n: usize) ?*anyopaque;
extern "c" fn free(p: ?*anyopaque) void;
extern "c" fn time(t: ?*i64) i64;

// A response in flight, its bytes right after it. who is the connection's receive tag, so a late completion can
// tell whether the connection it belongs to is still the same one.
const Sop = extern struct { who: u64, len: u32, off: u32, cap: u32, pad: u32 = 0 };
inline fn sopd(o: *Sop) [*]u8 {
    return @as([*]u8, @ptrCast(o)) + @sizeOf(Sop);
}
const Conn = extern struct {
    in: ?[*]u8, // partial request, or input held while a send is in flight
    inlen: u32,
    incap: u32,
    q: ?*Sop, // responses queued behind the send in flight
    out: ?*Sop, // the send in flight
    gen: u32,
    open: bool,
    closing: bool,
    sending: bool,
    eof: bool,
    sent100: bool,
    queued: bool,
    ready: bool,
};
// Accepted sockets live in each ring's registered file table, not in the process descriptor table.
// A table can't be larger than RLIMIT_NOFILE, so more rings are added as tables fill up.
const Ring = struct {
    r: linux.IoUring,
    efd: i32, // what io_uring_enter is given: the registered ring index if that worked, else the descriptor
    eflags: u32,
    c: [*]Conn,
    n: u32, // open connections
    pend: u32, // accepts in flight
    reads: u32, // single-shot reads in flight
    br: *linux.io_uring_buf_ring,
    bufs: [*]u8,
};
var R: [MAXRINGS]Ring = undefined;
var nrings: usize = 0;
var lfd: [NLISTEN]i32 = undefined;
var tsize: u32 = 0; // slots per ring
var db: ?*c.sqlite3 = null;
var st_feed: ?*c.sqlite3_stmt = null;
var st_post: ?*c.sqlite3_stmt = null;
var st_ins: ?*c.sqlite3_stmt = null;
var st_like: ?*c.sqlite3_stmt = null;
var st_one: ?*c.sqlite3_stmt = null;
var st_begin: ?*c.sqlite3_stmt = null;
var st_commit: ?*c.sqlite3_stmt = null;
var st_rollback: ?*c.sqlite3_stmt = null;
var started: i64 = 0;
var now_s: i64 = 0;

// user_data: low 3 bits tag the operation. A send's user_data is its Sop pointer (tag 0).
const T_SEND = 0;
const T_ACCEPT = 1;
const T_RECV = 2;
const T_NOP = 3;
const T_READ = 4;
const T_WAL = 5;
fn recvUd(k: usize, slot: u32, gen: u32) u64 {
    return T_RECV | @as(u64, k) << 3 | @as(u64, slot) << 8 | @as(u64, gen) << 32;
}
inline fn K(who: u64) usize {
    return @intCast(who >> 3 & 31);
}
inline fn SLOT(who: u64) u32 {
    return @intCast(who >> 8 & 0xffffff);
}
fn errno(rc: usize) i32 { // 0, or the errno of a raw syscall result
    const s: isize = @bitCast(rc);
    return if (s < 0 and s > -4096) @intCast(-s) else 0;
}
const E_NOBUFS: i32 = @intFromEnum(linux.E.NOBUFS);
const E_AGAIN: i32 = @intFromEnum(linux.E.AGAIN);

// Hot regions sit on huge pages where the kernel offers them (Ubuntu's default THP mode is madvise): a TLB miss
// on a VM walks two page tables.
fn hugeAlloc(n0: usize) [*]u8 {
    const hp: usize = 2 << 20;
    const n = (n0 + hp - 1) & ~(hp - 1);
    const rc = linux.mmap(null, n + hp, linux.PROT.READ | linux.PROT.WRITE, .{ .TYPE = .PRIVATE, .ANONYMOUS = true }, -1, 0);
    if (errno(rc) != 0) die("mmap failed", .{});
    const a = (rc + hp - 1) & ~(hp - 1);
    _ = linux.madvise(@ptrFromInt(a), n, linux.MADV.HUGEPAGE);
    return @ptrFromInt(a);
}
fn die(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print(fmt ++ "\n", args);
    std.process.exit(1);
}

// Growable list of connection tags; the read and ready queues pop from the front.
const List = struct {
    a: [*]u64 = undefined,
    head: usize = 0,
    n: usize = 0,
    cap: usize = 0,
    fn push(l: *List, v: u64) void {
        if (l.n == l.cap) {
            l.cap = if (l.cap > 0) l.cap * 2 else 1024;
            const p = realloc(if (l.n > 0) @ptrCast(l.a) else null, l.cap * 8) orelse die("out of memory", .{});
            l.a = @ptrCast(@alignCast(p));
        }
        l.a[l.n] = v;
        l.n += 1;
    }
    fn pop(f: *List) u64 {
        const v = f.a[f.head];
        f.head += 1;
        if (f.head == f.n) {
            f.head = 0;
            f.n = 0;
        } else if (f.head >= 65536 and f.head * 2 >= f.n) {
            f.n -= f.head;
            std.mem.copyForwards(u64, f.a[0..f.n], f.a[f.head..][0..f.n]);
            f.head = 0;
        }
        return v;
    }
    fn take(l: *List) List { // the list, leaving this one empty
        const t = l.*;
        l.* = .{};
        return t;
    }
    fn deinit(l: *List) void {
        if (l.cap > 0) free(@ptrCast(l.a));
    }
};

// ---------- output: one arena per batch, copied out to sends after COMMIT ----------
var arena: [*]u8 = undefined;
var alen: usize = 0;
const Seg = struct { who: u64, off: u32, len: u32 };
var segs: [MAXSEG]Seg = undefined;
var nsegs: usize = 0;
var deferred: List = .{};
var held: List = .{};
var eofs: List = .{};
var limbo: List = .{};
var arena_full = false;
var in_txn = false;
var txn_writes = false;
var batch_failed = false;
var hold_hit = false;
var hold_posts = false;

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
const VL = std.simd.suggestVectorLength(u8) orelse 16;
// JSON string contents, escaped. Output text almost never needs escaping, so a vector test skips ahead to the
// first byte that does, and most strings go out in one copy.
fn putjs(s: []const u8) void {
    const Vec = @Vector(VL, u8);
    var run: usize = 0;
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        if (s.len - i >= VL) {
            const v: Vec = s[i..][0..VL].*;
            const esc = @intFromBool(v < @as(Vec, @splat(0x20))) | @intFromBool(v == @as(Vec, @splat('"'))) |
                @intFromBool(v == @as(Vec, @splat('\\')));
            i += std.simd.firstTrue(esc != @as(@Vector(VL, u1), @splat(0))) orelse {
                i += VL - 1;
                continue;
            };
        }
        const ch = s[i];
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
test "putjs matches a byte-at-a-time escape" {
    const buf = try std.testing.allocator.alloc(u8, 1 << 16);
    defer std.testing.allocator.free(buf);
    arena = buf.ptr;
    var prng = std.Random.DefaultPrng.init(7);
    const rnd = prng.random();
    var s: [300]u8 = undefined;
    var want: [2000]u8 = undefined;
    for (0..20000) |_| {
        const n = rnd.uintLessThan(usize, s.len);
        for (s[0..n]) |*x| x.* = if (rnd.uintLessThan(u8, 8) == 0) "\"\\\n\x01\x1f\t"[rnd.uintLessThan(u8, 6)] else rnd.int(u8);
        var w: usize = 0;
        for (s[0..n]) |ch| {
            const e: []const u8 = switch (ch) {
                '"' => "\\\"",
                '\\' => "\\\\",
                '\n' => "\\n",
                '\r' => "\\r",
                '\t' => "\\t",
                0...8, 11, 12, 14...0x1f => &[_]u8{ '\\', 'u', '0', '0', hexd[ch >> 4], hexd[ch & 15] },
                else => &[_]u8{ch},
            };
            @memcpy(want[w..][0..e.len], e);
            w += e.len;
        }
        alen = 0;
        putjs(s[0..n]);
        try std.testing.expectEqualSlices(u8, want[0..w], arena[0..alen]);
    }
}

// response head with a blank Content-Length that finish() fills in
var body_start: usize = 0;
var cl_pos: usize = 0;
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
    cl_pos = alen - 10;
    body_start = alen;
}
fn finish() void {
    var n = alen - body_start;
    var p = cl_pos + 6;
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
var hm_in: [8]u32 = undefined; // HMAC key schedule: hash state after absorbing key^ipad / key^opad
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
fn b64init() void {
    for ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 0..) |ch, i| b64t[ch] = @intCast(i);
}
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
    var aj = Js{ .s = hs[0..hw[0].ve], .p = hw[0].v };
    const al = jstr(&aj, &alg);
    if (al != 5 or !std.mem.eql(u8, alg[0..5], "HS256")) return bad;
    const ps = pl[0..@intCast(pn)];
    var pw = [_]Want{ .{ .key = "exp" }, .{ .key = "nbf" }, .{ .key = "sub" }, .{ .key = "username" } };
    if (!jdoc(ps, &pw) or !pw[0].found) return bad; // exp is required
    for (0..2) |i| if (pw[i].found) { // exp / nbf must be numbers, checked against now
        const span = ps[pw[i].v..pw[i].ve];
        if (span[0] != '-' and !isDigit(span[0])) return bad;
        var nb: [64]u8 = undefined;
        if (span.len >= nb.len) return bad;
        @memcpy(nb[0..span.len], span);
        nb[span.len] = 0;
        const v = strtod(@ptrCast(&nb), null);
        const now: f64 = @floatFromInt(now_s);
        if (if (i == 0) now >= v else now < v) return bad;
    };
    if (!pw[2].found or ps[pw[2].v] != '"' or !pw[3].found or ps[pw[3].v] != '"') return payload;
    var sb: [24]u8 = undefined;
    var sj = Js{ .s = ps[0..pw[2].ve], .p = pw[2].v };
    const sl = jstr(&sj, &sb);
    if (sl <= 0 or sl > 15) return payload;
    var v: i64 = 0;
    for (sb[0..@intCast(sl)]) |d| {
        if (!isDigit(d)) return payload;
        v = v * 10 + (d - '0');
    }
    if (v <= 0) return payload;
    var uj = Js{ .s = ps[0..pw[3].ve], .p = pw[3].v };
    const ul = jstr(&uj, &a.uname);
    if (ul < 0) return payload;
    a.uid = v;
    a.unl = @intCast(ul);
    return null;
}

// ---------- database ----------
// Reads walk SQLite's own B-tree cursors (through src/btree.c), inside the batch's transaction; writes and anything
// unusual use SQL.
const C_POSTS = 0;
const C_USERS = 1;
const C_FEED = 2;
const C_LIKES = 3;
const TdsPage = extern struct {
    data: [*]const u8,
    cell_idx: [*]const u8,
    end: [*]const u8,
    mask: u16,
    ncell: u16,
    max_local: u16,
    ix: u16,
    child_ptr: u8,
    leaf: u8,
};
extern fn tds_setup(d: ?*c.sqlite3, roots: *const [4]c_uint) c_int;
extern fn tds_begin() c_int;
extern fn tds_close() void;
extern fn tds_first(k: c_int, empty: *c_int) c_int;
extern fn tds_next(k: c_int) c_int;
extern fn tds_seek(k: c_int, id: i64) c_int;
extern fn tds_record(k: c_int, n: *c_uint) ?[*]const u8;
extern fn tds_likes_from(lo: i64) c_int;
extern fn tds_likes_page(v: *TdsPage) void;
extern fn tds_likes_set_ix(ix: u16) void;
extern fn tds_pagecache(mem: ?*anyopaque, npg: c_int) c_int;
var bt_ok = false;
var cur_open = false;

fn closeCursors() void {
    if (!cur_open) return;
    tds_close();
    cur_open = false;
}
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
    if (rc != c.SQLITE_DONE) return false;
    if (bt_ok) {
        if (tds_begin() == 0) {
            _ = step(st_rollback);
            reset(st_rollback);
            return false;
        }
        cur_open = true;
    }
    in_txn = true;
    return true;
}
fn wstep(s: ?*c.sqlite3_stmt) c_int { // a write inside the batch transaction
    const rc = step(s);
    if (rc == c.SQLITE_ROW or rc == c.SQLITE_DONE) txn_writes = true;
    return rc;
}
fn dbFail() void { // a failed statement may have rolled the whole batch back
    if (in_txn and c.sqlite3_get_autocommit(db) != 0) {
        closeCursors();
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
fn putPostRow(s: ?*c.sqlite3_stmt) void {
    put("{\"id\":");
    putu(@bitCast(c.sqlite3_column_int64(s, 0)));
    put(",\"body\":\"");
    putjs(col(s, 1));
    put("\",\"created_at\":\"");
    put(col(s, 2));
    put("\",\"author\":\"");
    putjs(col(s, 3));
    put("\",\"like_count\":");
    putu(@bitCast(c.sqlite3_column_int64(s, 4)));
    put("}");
}

// A record's columns, decoded in place (SQLite's documented record format). Anything unexpected makes the caller
// fall back to SQL.
const Col = struct { t: u32, d: []const u8 };
fn varint32(p: [*]const u8, v: *u32) u32 { // SQLite varint; values past 32 bits read as 0xffffffff, as getVarint32 does
    if (p[0] < 0x80) {
        v.* = p[0];
        return 1;
    }
    var x: u64 = 0;
    var i: u32 = 0;
    while (i < 8) : (i += 1) {
        x = x << 7 | (p[i] & 0x7f);
        if (p[i] < 0x80) break;
    }
    if (i == 8) {
        x = x << 8 | p[8];
        i = 9;
    } else i += 1;
    v.* = if (x > 0xffffffff) 0xffffffff else @intCast(x);
    return i;
}
const small_sizes = [10]u32{ 0, 1, 2, 3, 4, 6, 8, 8, 0, 0 };
fn recParse(rec: []const u8, out: []Col) bool {
    if (rec.len == 0) return false;
    var hs: u32 = undefined;
    var i = varint32(rec.ptr, &hs);
    if (hs > rec.len) return false;
    var d: usize = hs;
    for (out) |*o| {
        if (i >= hs) return false;
        var t: u32 = undefined;
        i += varint32(rec.ptr + i, &t);
        if (t == 10 or t == 11) return false;
        const l: usize = if (t >= 12) (t - 12) / 2 else small_sizes[t];
        if (d + l > rec.len) return false;
        o.* = .{ .t = t, .d = rec[d..][0..l] };
        d += l;
    }
    return true;
}
fn recCols(k: c_int, out: []Col) bool {
    var n: c_uint = 0;
    const p = tds_record(k, &n) orelse return false; // spills onto overflow pages
    return recParse(p[0..n], out);
}
fn colText(x: *const Col) bool {
    return x.t >= 13 and x.t & 1 == 1;
}
fn colInt(x: *const Col, v: *i64) bool {
    if (x.t == 8 or x.t == 9) {
        v.* = x.t - 8;
        return true;
    }
    if (x.t < 1 or x.t > 6) return false;
    var r: i64 = @as(i8, @bitCast(x.d[0]));
    for (x.d[1..]) |b| r = r << 8 | b;
    v.* = r;
    return true;
}
test "record decoding" {
    // header 4: types 1 (int8), 13+2*3 (text 3), 6 (int64)
    const rec = [_]u8{ 4, 1, 19, 6, 0xfe, 'a', 'b', 'c', 0, 0, 0, 0, 0, 0, 1, 0 };
    var cs: [3]Col = undefined;
    try std.testing.expect(recParse(&rec, &cs));
    var v: i64 = 0;
    try std.testing.expect(colInt(&cs[0], &v) and v == -2);
    try std.testing.expect(colText(&cs[1]) and std.mem.eql(u8, cs[1].d, "abc"));
    try std.testing.expect(colInt(&cs[2], &v) and v == 256);
    try std.testing.expect(!recParse(rec[0..10], &cs));
    var x: u32 = 0;
    try std.testing.expect(varint32(&[_]u8{ 0x81, 0x00 }, &x) == 2 and x == 128);
}
// Tallies the likes index entries in cells first..last of a page into n[post_id - lo]. Returns 1 to keep walking,
// 0 on reaching a post past hi, -1 on a cell this code won't decode.
fn tally(pg: *const TdsPage, first: i32, last: i32, lo: i64, hi: i64, n: []u32) i32 {
    var i = first;
    while (i <= last) : (i += 1) {
        const off = std.mem.readInt(u16, pg.cell_idx[@as(usize, @intCast(2 * i))..][0..2], .big) & pg.mask;
        const cell = pg.data + off + pg.child_ptr;
        var size: u32 = undefined;
        const skip = varint32(cell, &size);
        if (size > pg.max_local or @intFromPtr(cell) + skip + size > @intFromPtr(pg.end)) return -1;
        var key: [1]Col = undefined;
        var post: i64 = 0;
        if (!recParse(cell[skip..][0..size], &key) or !colInt(&key[0], &post)) return -1;
        if (post > hi) return 0;
        if (post >= lo) n[@intCast(post - lo)] += 1;
    }
    return 1;
}
// Likes for posts lo..hi, added to n[post - lo], a page at a time. Returns 1 done, 0 for SQL to answer, -1 on error.
fn countLikes(lo: i64, hi: i64, n: []u32) i32 {
    var on = tds_likes_from(lo);
    while (on == 1) {
        var pg: TdsPage = undefined;
        tds_likes_page(&pg);
        const last: i32 = if (pg.leaf != 0) @as(i32, pg.ncell) - 1 else pg.ix; // on an interior page the entry under the cursor is one key
        const more = tally(&pg, pg.ix, last, lo, hi, n);
        if (more <= 0) return if (more < 0) 0 else 1;
        tds_likes_set_ix(@truncate(@as(u32, @bitCast(last))));
        const rc = tds_next(C_LIKES);
        on = if (rc == c.SQLITE_OK) 1 else if (rc == c.SQLITE_DONE) 0 else -1;
    }
    return if (on == 0) 1 else -1;
}
fn putRow(id: i64, body: *const Col, ca: []const u8, name: *const Col, likes: u32) void {
    put("{\"id\":");
    putu(@bitCast(id));
    put(",\"body\":\"");
    putjs(body.d);
    put("\",\"created_at\":\"");
    put(ca);
    put("\",\"author\":\"");
    putjs(name.d);
    put("\",\"like_count\":");
    putu(likes);
    put("}");
}
// Author of a post: 1 found, 0 use SQL, -1 error. The cell stays valid while the users cursor doesn't move.
fn rowAuthor(uid: i64, name: *Col) i32 {
    const rc = tds_seek(C_USERS, uid);
    if (rc <= 0) return rc;
    var cs: [2]Col = undefined;
    if (!recCols(C_USERS, &cs) or !colText(&cs[1])) return 0;
    name.* = cs[1];
    return 1;
}

const FEED_N = 20;
const FEED_SPAN = 512; // ids this close together share one walk of the likes index
var ftmp: [BATCH_OUT]u8 = undefined;
// The 20 newest off posts_created_at_id_idx, then the rows in ascending id order (consecutive ids are one step
// along a leaf), then one likes walk. Returns 1 when the feed was written, 0 to use SQL, -1 on a database error.
fn feedBtree(close: bool) i32 {
    if (!bt_ok) return 0;
    var ids: [FEED_N]i64 = undefined;
    var ca: [FEED_N][32]u8 = undefined;
    var cal: [FEED_N]usize = undefined;
    var likes = [_]u32{0} ** FEED_N;
    var ord: [FEED_N]usize = undefined;
    var n: usize = 0;
    var empty: c_int = 1;
    if (tds_first(C_FEED, &empty) != c.SQLITE_OK) return -1;
    if (empty == 0) while (true) {
        var cs: [3]Col = undefined;
        if (!recCols(C_FEED, &cs) or !colText(&cs[0]) or cs[0].d.len > 32 or !colInt(&cs[1], &ids[n])) return 0;
        @memcpy(ca[n][0..cs[0].d.len], cs[0].d);
        cal[n] = cs[0].d.len;
        n += 1;
        if (n == FEED_N) break;
        const rc = tds_next(C_FEED);
        if (rc == c.SQLITE_DONE) break;
        if (rc != c.SQLITE_OK) return -1;
    };
    for (0..n) |i| { // newest first is normally descending id, so this is usually just a reversal
        var j = i;
        const v = n - 1 - i;
        while (j > 0 and ids[ord[j - 1]] > ids[v]) : (j -= 1) ord[j] = ord[j - 1];
        ord[j] = v;
    }
    if (n > 0) {
        const lo = ids[ord[0]];
        const hi = ids[ord[n - 1]];
        if (hi - lo < FEED_SPAN) {
            var span: [FEED_SPAN]u32 = undefined;
            @memset(span[0..@intCast(hi - lo + 1)], 0);
            const rc = countLikes(lo, hi, &span);
            if (rc <= 0) return rc;
            for (0..n) |i| likes[i] = span[@intCast(ids[i] - lo)];
        } else for (0..n) |i| {
            const rc = countLikes(ids[i], ids[i], likes[i..][0..1]);
            if (rc <= 0) return rc;
        }
    }
    const mark = alen;
    var at: [FEED_N]usize = undefined;
    var len: [FEED_N]usize = undefined;
    beginResp(200, close);
    put("{\"posts\":[");
    const rows = alen;
    for (ord[0..n]) |i| {
        var rc = tds_seek(C_POSTS, ids[i]);
        var cs: [3]Col = undefined;
        var name: Col = undefined;
        var uid: i64 = 0;
        if (rc < 0) {
            alen = mark;
            return -1;
        }
        if (rc == 0 or !recCols(C_POSTS, &cs) or !colText(&cs[2]) or !colInt(&cs[1], &uid)) {
            alen = mark;
            return 0;
        }
        rc = rowAuthor(uid, &name);
        if (rc <= 0) {
            alen = mark;
            return rc;
        }
        at[i] = alen - rows;
        putRow(ids[i], &cs[2], ca[i][0..cal[i]], &name, likes[i]);
        len[i] = alen - rows - at[i];
    }
    const total = alen - rows;
    @memcpy(ftmp[0..total], arena[rows..alen]); // back into feed order
    alen = rows;
    for (0..n) |i| {
        if (i > 0) put(",");
        put(ftmp[at[i]..][0..len[i]]);
    }
    put("]}");
    finish();
    return 1;
}
// GET /posts/:id. Returns 1 when written, 0 to use SQL (which also answers a missing post), -1 on a database error.
fn postBtree(id: i64, close: bool) i32 {
    if (!bt_ok) return 0;
    var rc = tds_seek(C_POSTS, id);
    var cs: [4]Col = undefined;
    var name: Col = undefined;
    var uid: i64 = 0;
    var likes = [1]u32{0};
    if (rc <= 0) return rc;
    if (!recCols(C_POSTS, &cs) or !colText(&cs[2]) or !colText(&cs[3]) or !colInt(&cs[1], &uid)) return 0;
    rc = rowAuthor(uid, &name);
    if (rc <= 0) return rc;
    rc = countLikes(id, id, &likes);
    if (rc <= 0) return rc;
    beginResp(200, close);
    put("{\"post\":");
    putRow(id, &cs[2], cs[3].d, &name, likes[0]);
    put("}");
    finish();
    return 1;
}

// ---------- request handling ----------
fn parseId(s: []const u8) i64 { // positive decimal integer, else -1 (huge ids saturate: not found)
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
        const fr = feedBtree(close);
        if (fr != 0) {
            if (fr < 0) {
                dbFail();
                err(500, "internal server error", close);
            }
            return;
        }
        const mark = alen;
        beginResp(200, close);
        put("{\"posts\":[");
        var i: usize = 0;
        var rc = step(st_feed);
        while (rc == c.SQLITE_ROW) : (rc = step(st_feed)) {
            if (i > 0) put(",");
            i += 1;
            putPostRow(st_feed);
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
            const pr = postBtree(id, close);
            if (pr != 0) {
                if (pr < 0) {
                    dbFail();
                    err(500, "internal server error", close);
                }
                return;
            }
            _ = c.sqlite3_bind_int64(st_post, 1, id);
            const mark = alen;
            const rc = step(st_post);
            if (rc == c.SQLITE_ROW) {
                beginResp(200, close);
                put("{\"post\":");
                putPostRow(st_post);
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
            var rc: c_int = if (bt_ok) tds_seek(C_POSTS, id) else 1; // without B-tree reads the statement's own EXISTS decides
            if (rc < 0) {
                dbFail();
                return err(500, "internal server error", close);
            }
            if (rc == 0) return err(404, "post not found", close);
            _ = c.sqlite3_bind_int64(st_like, 1, a.uid);
            _ = c.sqlite3_bind_int64(st_like, 2, id);
            rc = wstep(st_like);
            reset(st_like);
            if (rc != c.SQLITE_DONE) {
                dbFail();
                return err(500, "internal server error", close);
            }
            const created = c.sqlite3_changes(db) > 0;
            if (!created and !bt_ok) {
                _ = c.sqlite3_bind_int64(st_post, 1, id);
                rc = step(st_post);
                reset(st_post);
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
        var bj = Js{ .s = body[0..w[0].ve], .p = w[0].v };
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
        if (s == se or txt[s] == 0) return err(400, "body is required", close); // length() stops at a NUL, so the CHECK would fail
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
        putu(@bitCast(c.sqlite3_column_int64(st_ins, 0)));
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

var no_conn = false;
var sent100: *bool = &no_conn; // the current connection's "100 Continue already sent" flag

// Serves every complete request in buf. Returns bytes consumed, or -1 to drop the connection.
fn serve(buf: []u8, closing: *bool) isize {
    const eq = std.ascii.eqlIgnoreCase;
    var used: usize = 0;
    while (used < buf.len and !closing.*) {
        const r = buf[used..];
        if (alen + 2 * MAXREQ > BATCH_OUT or nsegs == MAXSEG) { // the rest waits for the next batch
            arena_full = true;
            break;
        }
        if (hold_posts and r[0] == 'P') { // a POST waits, unread, while the last of the log is copied back
            hold_hit = true;
            break;
        }
        const he = if (memmem(r.ptr, r.len, "\r\n\r\n", 4)) |q| @intFromPtr(q) - @intFromPtr(r.ptr) else {
            if (r.len > MAXREQ) return -1;
            break;
        };
        const le = std.mem.indexOfScalar(u8, r[0 .. he + 2], '\r').?; // there is one at he
        const line = r[0..le];
        const sp1 = std.mem.indexOfScalar(u8, line, ' ') orelse return -1;
        const sp2 = std.mem.indexOfScalarPos(u8, line, sp1 + 1, ' ') orelse return -1;
        var close = std.mem.eql(u8, line[sp2 + 1 ..], "HTTP/1.0");
        var authh: ?[]const u8 = null;
        var clen: usize = 0;
        var expect = false;
        var h = le + 2;
        while (h < he + 2) {
            const hl = std.mem.indexOfScalarPos(u8, r[0 .. he + 2], h, '\r') orelse return -1; // a bare CR just before the end of the head
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
            if (expect and body == r.len and !sent100.*) {
                put("HTTP/1.1 100 Continue\r\n\r\n");
                sent100.* = true;
            }
            break;
        }
        sent100.* = false;
        handle(line[0..sp1], line[sp1 + 1 .. sp2], authh, r[body .. body + clen], close);
        if (close) closing.* = true;
        used += body + clen;
    }
    return @intCast(used);
}

// ---------- rings: what liburing would do, on std.os.linux.IoUring ----------
fn enter(k: usize, nsub: u32, want: u32, flags: u32, arg: ?*const linux.io_uring_getevents_arg) usize {
    const r = &R[k];
    var f = flags | r.eflags;
    if (arg != null) f |= linux.IORING_ENTER_EXT_ARG;
    return linux.syscall6(.io_uring_enter, @bitCast(@as(isize, r.efd)), nsub, want, f, @intFromPtr(arg), if (arg != null) @sizeOf(linux.io_uring_getevents_arg) else 0);
}
fn submit(k: usize) void { // io_uring_submit: enters only with something to submit
    const n = R[k].r.flush_sq();
    if (n > 0) _ = enter(k, n, 0, 0, null);
}
fn submitAndGetEvents(k: usize) usize {
    return enter(k, R[k].r.flush_sq(), 0, linux.IORING_ENTER_GETEVENTS, null);
}
fn sqSpaceLeft(k: usize) usize {
    return R[k].r.sq.sqes.len - R[k].r.sq_ready();
}
fn sqe(k: usize) *linux.io_uring_sqe {
    while (true) {
        const s = R[k].r.get_sqe() catch {
            submit(k);
            continue;
        };
        return s;
    }
}

// ---------- connections ----------
fn connOf(who: u64) ?*Conn { // null once that connection has closed
    const cn = &R[K(who)].c[SLOT(who)];
    return if (cn.open and cn.gen == @as(u32, @truncate(who >> 32))) cn else null;
}
var readq: List = .{}; // connections to read
var readyq: List = .{}; // connections with bytes read and not yet answered
fn connClose(who: u64) void {
    const k = K(who);
    const slot = SLOT(who);
    const r = &R[k];
    const cn = &r.c[slot];
    free(cn.in);
    free(cn.q); // a send still in flight frees its own buffer when it completes
    const out = if (cn.sending) cn.out else null;
    const g = cn.gen +% 1;
    cn.* = std.mem.zeroes(Conn);
    cn.gen = g;
    r.n -= 1;
    if (sqSpaceLeft(k) < 3) submit(k); // keep the chain in one submission
    // End the pending receive (and send), then free the slot. They are cancelled by their tags, which the
    // kernel finds in a hash table; cancelling by descriptor would scan every pending operation on the
    // ring, one per open connection.
    var s = sqe(k);
    s.prep_cancel(who, 0);
    s.flags |= linux.IOSQE_IO_HARDLINK;
    s.user_data = T_NOP;
    if (out) |o| {
        s = sqe(k);
        s.prep_cancel(@intFromPtr(o), 0);
        s.flags |= linux.IOSQE_IO_HARDLINK;
        s.user_data = T_NOP;
    }
    s = sqe(k);
    s.prep_close_direct(slot);
    s.user_data = T_NOP;
}
fn queueRead(who: u64, cn: *Conn) void {
    if (!cn.queued) {
        cn.queued = true;
        readq.push(who);
    }
}
fn armRecv(who: u64) void { // multishot: one completion per chunk, into a provided buffer
    const s = sqe(K(who));
    s.prep_rw(.RECV, @intCast(SLOT(who)), 0, 0, 0);
    s.ioprio = linux.IORING_RECV_MULTISHOT;
    s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
    s.buf_index = 0;
    s.user_data = who;
}
fn sendOp(o: *Sop) void {
    const s = sqe(K(o.who));
    s.prep_send(@intCast(SLOT(o.who)), sopd(o)[o.off..o.len], linux.MSG.NOSIGNAL);
    s.flags |= linux.IOSQE_FIXED_FILE;
    s.user_data = @intFromPtr(o);
}
// One send in flight per connection keeps responses in order; later ones queue behind it.
// Sends finish inline, so a small LIFO pool keeps handing out the buffer most recently written, still in cache.
const POOL_CAP: u32 = 8192 - @sizeOf(Sop);
const POOL_MAX = if (COMBO) 2048 else 512; // 2048 >= ANSWERS: a full pass never mallocs a send buffer
var pool: [POOL_MAX]*Sop = undefined;
var npool: usize = 0;
fn sopNew(n: u32) *Sop {
    if (n <= POOL_CAP and npool > 0) {
        npool -= 1;
        return pool[npool];
    }
    const p = malloc(@sizeOf(Sop) + if (n <= POOL_CAP) POOL_CAP else n) orelse die("out of memory", .{});
    return @ptrCast(@alignCast(p));
}
fn sopFree(o: *Sop) void {
    if (o.cap == POOL_CAP and npool < POOL_MAX) {
        pool[npool] = o;
        npool += 1;
    } else free(o);
}
fn sendBytes(who: u64, cn: *Conn, p: []const u8) void {
    const n: u32 = @intCast(p.len);
    if (!cn.sending) {
        const o = sopNew(n);
        o.* = .{ .who = who, .len = n, .off = 0, .cap = if (n <= POOL_CAP) POOL_CAP else n };
        @memcpy(sopd(o)[0..n], p);
        cn.sending = true;
        cn.out = o;
        return sendOp(o);
    }
    var q = cn.q;
    if (q == null or q.?.cap - q.?.len < n) {
        const len: u32 = if (q) |x| x.len else 0;
        const cap = (len + n) * 2;
        const nq: *Sop = @ptrCast(@alignCast(realloc(q, @sizeOf(Sop) + cap) orelse die("out of memory", .{})));
        nq.* = .{ .who = who, .len = len, .off = 0, .cap = cap };
        cn.q = nq;
        q = nq;
    }
    const qq = q.?;
    @memcpy(sopd(qq)[qq.len..][0..n], p);
    qq.len += n;
}

fn process(who: u64, cn: *Conn, buf: []u8) void {
    const start = alen;
    var closing = false;
    arena_full = false;
    hold_hit = false;
    sent100 = &cn.sent100;
    const ui = serve(buf, &closing);
    if (ui < 0) {
        alen = start;
        return connClose(who);
    }
    const used: usize = @intCast(ui);
    if (alen > start) {
        segs[nsegs] = .{ .who = who, .off = @intCast(start), .len = @intCast(alen - start) };
        nsegs += 1;
    }
    cn.closing = closing;
    const left = if (closing) 0 else buf.len - used;
    if (left > 0) {
        if (cn.in == null or buf.ptr != cn.in.?) {
            cn.incap = @intCast(left + BSZ);
            const p: [*]u8 = @ptrCast(malloc(cn.incap) orelse die("out of memory", .{}));
            @memcpy(p[0..left], buf[used..]);
            cn.in = p;
        } else if (used > 0) std.mem.copyForwards(u8, cn.in.?[0..left], cn.in.?[used..][0..left]);
        cn.inlen = @intCast(left);
        if (arena_full) deferred.push(who);
        if (hold_hit) held.push(who);
    } else if (cn.in != null) {
        free(cn.in);
        cn.in = null;
        cn.inlen = 0;
        cn.incap = 0;
    }
    if (alen == start and (closing or (cn.eof and !cn.sending and !arena_full and !hold_hit))) connClose(who);
}
// After a stall every connection has a request waiting. Letting them sit costs a socket buffer each (3 to 4 KB, toward
// TCP's limit) and answering them all makes one very long pass, so a pass answers ANSWERS and reads READS: the rest
// wait on their connection, in a copy the size of the request, on a first-in, first-out list.
const ANSWERS = 1024;
const READS = 4096;
var answers_left: u32 = ANSWERS;
fn onData(who: u64, cn: *Conn, p: []const u8) void {
    if (cn.closing) return; // answering Connection: close, anything after it is ignored
    const later = cn.ready or answers_left == 0;
    if (!later) answers_left -= 1;
    if (!later and cn.inlen == 0 and !cn.sending) return process(who, cn, @constCast(p));
    const n: u32 = @intCast(p.len);
    if (cn.incap - cn.inlen < n) {
        cn.incap = (cn.inlen + n) * 2;
        cn.in = @ptrCast(realloc(cn.in, cn.incap) orelse die("out of memory", .{}));
    }
    @memcpy(cn.in.?[cn.inlen..][0..n], p);
    cn.inlen += n;
    if (cn.inlen > 4 * MAXREQ) return connClose(who); // keeps sending but won't read its replies
    if (later) {
        if (!cn.ready) {
            cn.ready = true;
            readyq.push(who);
        }
    } else if (!cn.sending) process(who, cn, cn.in.?[0..cn.inlen]);
}
fn runReady() void { // with the answers this pass has left, what earlier passes read, oldest first
    while (answers_left > 0 and readyq.n > 0) {
        const who = readyq.pop();
        const cn = connOf(who) orelse continue;
        cn.ready = false;
        if (cn.inlen > 0 and !cn.sending) {
            answers_left -= 1;
            process(who, cn, cn.in.?[0..cn.inlen]);
        }
    }
}
fn sendDone(o: *Sop, res: i32) void {
    const who = o.who;
    const cn = connOf(who);
    if (cn != null and res > 0) {
        o.off += @intCast(res);
        if (o.off < o.len) return sendOp(o); // short send: the rest
    }
    // A closed connection's cancel may still be queued, naming this address: it stays unused until that has gone in.
    const cc = cn orelse return limbo.push(@intFromPtr(o));
    sopFree(o);
    if (res <= 0) {
        cc.sending = false;
        return connClose(who);
    }
    if (cc.q) |q| {
        cc.q = null;
        cc.out = q;
        return sendOp(q);
    }
    cc.sending = false;
    if (cc.closing) {
        connClose(who);
    } else if (cc.inlen > 0) {
        process(who, cc, cc.in.?[0..cc.inlen]); // input that arrived while sending
    } else if (cc.eof) connClose(who);
}

// Commits the batch, then sends every response produced in it.
fn flushBatch() void {
    if (in_txn) {
        closeCursors();
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
        const cn = connOf(s.who) orelse continue;
        if (batch_failed) { // the batch wasn't committed: never claim it was
            connClose(s.who);
            continue;
        }
        sendBytes(s.who, cn, arena[s.off..][0..s.len]);
    }
    nsegs = 0;
    alen = 0;
    batch_failed = false;
}

// Some Ubuntu 6.8 kernels (0-136 and later, Launchpad #2162843) refuse a registration whose reserved words
// are all zero and accept resv[0] = 1; every other kernel takes the zeroes, so those are tried first.
fn bufRing(fd: i32) ?*linux.io_uring_buf_ring {
    const sz = NBUF * @sizeOf(linux.io_uring_buf);
    const p = linux.mmap(null, sz, linux.PROT.READ | linux.PROT.WRITE, .{ .TYPE = .PRIVATE, .ANONYMOUS = true }, -1, 0);
    if (errno(p) != 0) return null;
    var reg = std.mem.zeroes(linux.io_uring_buf_reg);
    reg.ring_addr = p;
    reg.ring_entries = NBUF;
    var rc = linux.io_uring_register(fd, .REGISTER_PBUF_RING, &reg, 1);
    if (errno(rc) == @intFromEnum(linux.E.INVAL)) {
        reg.resv[0] = 1;
        rc = linux.io_uring_register(fd, .REGISTER_PBUF_RING, &reg, 1);
    }
    if (errno(rc) != 0) {
        _ = linux.munmap(@ptrFromInt(p), sz);
        return null;
    }
    const br: *linux.io_uring_buf_ring = @ptrFromInt(p);
    br.tail = 0;
    return br;
}
fn ringInit(k: usize) bool {
    const r = &R[k];
    // DEFER_TASKRUN on every ring: completion work runs when the loop asks, in one batch, never interrupting a pass.
    var p = std.mem.zeroInit(linux.io_uring_params, .{
        .flags = linux.IORING_SETUP_SUBMIT_ALL | linux.IORING_SETUP_SINGLE_ISSUER | linux.IORING_SETUP_CQSIZE |
            linux.IORING_SETUP_DEFER_TASKRUN,
        .cq_entries = 16384,
    });
    r.r = linux.IoUring.init_params(4096, &p) catch return false;
    r.r.register_files_sparse(tsize) catch {
        r.r.deinit();
        return false;
    };
    r.br = bufRing(r.r.fd) orelse {
        r.r.deinit();
        return false;
    };
    // A registered ring descriptor saves the descriptor lookup in every io_uring_enter.
    r.efd = r.r.fd;
    r.eflags = 0;
    var up = linux.io_uring_rsrc_update{ .offset = std.math.maxInt(u32), .resv = 0, .data = @intCast(r.r.fd) };
    if (linux.io_uring_register(r.r.fd, .REGISTER_RING_FDS, &up, 1) == 1) {
        r.efd = @intCast(up.offset);
        r.eflags = linux.IORING_ENTER_REGISTERED_RING;
    }
    r.c = @ptrCast(@alignCast(hugeAlloc(@as(usize, tsize) * @sizeOf(Conn))));
    r.bufs = hugeAlloc(NBUF * BSZ);
    for (0..NBUF) |i| linux.IoUring.buf_ring_add(r.br, r.bufs[i * BSZ ..][0..BSZ], @intCast(i), NBUF - 1, @intCast(i));
    linux.IoUring.buf_ring_advance(r.br, NBUF);
    r.n = 0;
    r.pend = 0;
    r.reads = 0;
    if (k > 0) std.debug.print("{d} connections: added ring {d}\n", .{ k * (tsize - 16), k });
    nrings = k + 1;
    return true;
}
// Accepts are single-shot, a few in flight, on the first ring with room. An accept that finds its
// table full has already taken the connection and can only drop it, so tables are never filled to the
// last slot (closes still in flight may hold a few) and a multishot accept can't be stopped in time.
// Each listener keeps its own share armed: connections are hashed to listeners, so a listener with none armed
// would leave its queue waiting while accepts armed on idle listeners never complete.
const ACCEPTS = 64; // per listener, rearmed once per loop pass, so this caps accepts per pass
var armed = [_]u32{0} ** NLISTEN;
fn armAccepts() void {
    var k: usize = 0;
    for (0..NLISTEN) |l| {
        while (armed[l] < ACCEPTS and k < MAXRINGS) {
            if (k == nrings and !ringInit(k)) return; // retried after the next event
            if (R[k].n + R[k].pend + 16 >= tsize) {
                k += 1;
                continue;
            }
            const s = sqe(k);
            s.prep_accept_direct(lfd[l], null, null, 0, linux.IORING_FILE_INDEX_ALLOC);
            s.user_data = T_ACCEPT | @as(u64, l) << 3;
            R[k].pend += 1;
            armed[l] += 1;
        }
    }
}
var wal_msg: u64 = 0;
var wal_outcome: u64 = 0;
var wal_replies: i32 = -1;
fn armWalMsg() void { // the worker's reply arrives as a completion on ring 0
    const s = sqe(0);
    s.prep_read(wal_replies, std.mem.asBytes(&wal_msg), 0);
    s.user_data = T_WAL;
}
fn giveBack(r: *Ring, bid: u16) void {
    linux.IoUring.buf_ring_add(r.br, r.bufs[@as(usize, bid) * BSZ ..][0..BSZ], bid, NBUF - 1, 0);
    linux.IoUring.buf_ring_advance(r.br, 1);
}
fn onCqe(k: usize, ud: u64, res: i32, f: u32) void {
    const r = &R[k];
    switch (ud & 7) {
        T_SEND => sendDone(@ptrFromInt(ud), res),
        T_ACCEPT => {
            r.pend -= 1;
            armed[@intCast(ud >> 3)] -= 1;
            if (res >= 0) {
                const cn = &r.c[@intCast(res)];
                cn.open = true;
                r.n += 1;
                armRecv(recvUd(k, @intCast(res), cn.gen));
            }
        },
        T_RECV => {
            if (f & linux.IORING_CQE_F_BUFFER != 0) {
                const bid: u16 = @intCast(f >> linux.IORING_CQE_BUFFER_SHIFT);
                if (res > 0) if (connOf(ud)) |cn| onData(ud, cn, r.bufs[@as(usize, bid) * BSZ ..][0..@intCast(res)]);
                giveBack(r, bid);
            }
            if (f & linux.IORING_CQE_F_MORE == 0) if (connOf(ud)) |cn| {
                // Out of buffers: rearming every receive at once would repeat that, so the rest is read in bounded rounds.
                if (res > 0 or res == -E_NOBUFS) {
                    queueRead(ud, cn);
                } else {
                    cn.eof = true;
                    eofs.push(ud); // the same pass may hold this client's last reply
                }
            };
        },
        T_WAL => {
            wal_outcome = if (res == 8) wal_msg else 1;
            armWalMsg();
        },
        T_READ => {
            const who = (ud & ~@as(u64, 7)) | T_RECV;
            r.reads -= 1;
            if (f & linux.IORING_CQE_F_BUFFER != 0) {
                const bid: u16 = @intCast(f >> linux.IORING_CQE_BUFFER_SHIFT);
                if (res > 0) if (connOf(who)) |cn| onData(who, cn, r.bufs[@as(usize, bid) * BSZ ..][0..@intCast(res)]);
                giveBack(r, bid);
            }
            const cn = connOf(who) orelse return;
            if (res == BSZ or res == -E_NOBUFS) {
                queueRead(who, cn); // a full buffer: there may be more
            } else if (res > 0 or res == -E_AGAIN) {
                armRecv(who); // drained: back to the multishot receive
            } else {
                cn.eof = true;
                eofs.push(who);
            }
        },
        else => {},
    }
}
fn drain(k: usize) u32 {
    const cq = &R[k].r.cq;
    const tail = @atomicLoad(u32, cq.tail, .acquire);
    var head = cq.head.*;
    const n = tail -% head;
    while (head != tail) : (head +%= 1) {
        const e = cq.cqes[head & cq.mask];
        onCqe(k, e.user_data, e.res, e.flags);
    }
    @atomicStore(u32, cq.head, tail, .release);
    return n;
}
// One MSG_DONTWAIT read per queued connection, never more on a ring than it has buffers. They finish inside the submit.
fn readRounds() u32 {
    var seen: u32 = 0;
    var budget: i32 = READS;
    while (budget > 0 and readq.n > 0) {
        var issued: i32 = 0;
        const stop = readq.n - readq.head;
        var i: usize = 0;
        while (i < stop and issued < budget) : (i += 1) {
            const who = readq.pop();
            const cn = connOf(who) orelse continue;
            const k = K(who);
            if (R[k].reads >= NBUF) {
                readq.push(who);
                continue;
            }
            cn.queued = false;
            R[k].reads += 1;
            issued += 1;
            const s = sqe(k);
            s.prep_rw(.RECV, @intCast(SLOT(who)), 0, 0, 0);
            s.rw_flags = linux.MSG.DONTWAIT;
            s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
            s.buf_index = 0;
            s.user_data = (who & ~@as(u64, 7)) | T_READ;
        }
        if (issued == 0) break;
        budget -= issued;
        for (0..nrings) |k| {
            _ = submitAndGetEvents(k);
            seen += drain(k);
        }
    }
    return seen;
}

const WAIT_N = 128; // completions a busy pass waits for, shared across the rings
const WAIT_NS = 2000000; // or this long, whichever is first
fn processList(l: *List) void { // requests released from a wait, then their batch
    var todo = l.take();
    for (todo.a[0..todo.n]) |w| if (connOf(w)) |cn| if (cn.inlen > 0 and !cn.sending) process(w, cn, cn.in.?[0..cn.inlen]);
    todo.deinit();
    flushBatch();
}
fn loop() noreturn {
    var seen: u32 = 0;
    while (true) {
        armAccepts();
        var open: u32 = R[0].n;
        for (1..nrings) |k| {
            open += R[k].n;
            _ = submitAndGetEvents(k);
        }
        // Busy, or more than one ring to look at: wait for ring 0's share of a batch or a short timeout, whichever is
        // first. The target is capped by the open connections so a few clients without think time never sit out the timer.
        var rc: usize = 0;
        if (readq.n > 0 or readyq.n > 0) {
            rc = submitAndGetEvents(0); // work is waiting here: no sleeping
        } else if (seen >= 4 or nrings > 1 or held.n > 0) {
            var want: u32 = if (seen >= 4) WAIT_N / @as(u32, @intCast(nrings)) + 1 else 1;
            if (want > open / 3 + 1) want = open / 3 + 1;
            var ts = linux.kernel_timespec{ .sec = 0, .nsec = WAIT_NS };
            var arg = std.mem.zeroes(linux.io_uring_getevents_arg);
            arg.sigmask_sz = linux.NSIG / 8;
            arg.ts = @intFromPtr(&ts);
            rc = enter(0, R[0].r.flush_sq(), want, linux.IORING_ENTER_GETEVENTS, &arg);
        } else rc = enter(0, R[0].r.flush_sq(), 1, linux.IORING_ENTER_GETEVENTS, null);
        for (limbo.a[0..limbo.n]) |o| sopFree(@ptrFromInt(o)); // those cancels are in now
        limbo.n = 0;
        switch (errno(rc)) {
            0, @intFromEnum(linux.E.INTR), @intFromEnum(linux.E.BUSY), @intFromEnum(linux.E.TIME), E_AGAIN => {},
            else => |e| die("io_uring_enter: errno {d}", .{e}),
        }
        now_s = time(null);
        seen = 0;
        answers_left = ANSWERS;
        // Requests that just arrived go first. After a stall the backlog has missed any latency target already; answered
        // first, it would make everything behind it miss too.
        for (0..nrings) |k| seen += drain(k);
        runReady();
        seen += readRounds();
        flushBatch();
        if (eofs.n > 0) { // clients that finished sending: close once what they sent has been answered
            for (eofs.a[0..eofs.n]) |w| {
                const cn = connOf(w) orelse continue;
                if (cn.sending) continue; // the send's completion takes it from here
                if (cn.inlen > 0) process(w, cn, cn.in.?[0..cn.inlen]) else connClose(w);
            }
            eofs.n = 0;
            flushBatch();
        }
        while (deferred.n > 0) processList(&deferred); // requests held back by a full arena
        walAfterCommit();
        if (held.n > 0 and walHoldOver()) processList(&held);
    }
}

fn prep(sql: [:0]const u8) ?*c.sqlite3_stmt {
    var s: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v3(db, sql.ptr, -1, c.SQLITE_PREPARE_PERSISTENT, &s, null) != c.SQLITE_OK)
        die("prepare: {s}: {s}", .{ std.mem.span(c.sqlite3_errmsg(db)), sql });
    return s;
}
fn rootpage(name: [:0]const u8) c_uint {
    var st: ?*c.sqlite3_stmt = null;
    var r: c_uint = 0;
    if (c.sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?", -1, &st, null) != c.SQLITE_OK) return 0;
    _ = c.sqlite3_bind_text(st, 1, name.ptr, -1, null);
    if (step(st) == c.SQLITE_ROW) r = @intCast(c.sqlite3_column_int(st, 0));
    _ = c.sqlite3_finalize(st);
    return r;
}
fn layoutIs(sql: [:0]const u8, want: []const u8) bool { // the decoders assume these exact columns
    var st: ?*c.sqlite3_stmt = null;
    var ok = false;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &st, null) != c.SQLITE_OK) return false;
    if (step(st) == c.SQLITE_ROW) ok = std.mem.eql(u8, col(st, 0), want);
    _ = c.sqlite3_finalize(st);
    return ok;
}
fn btreeSetup() void { // leaves bt_ok false (SQL for every read) on anything unexpected
    var roots: [4]c_uint = undefined;
    bt_ok = layoutIs("SELECT group_concat(name) FROM pragma_table_info('posts')", "id,user_id,body,created_at") and
        layoutIs("SELECT group_concat(name) FROM pragma_table_info('users')", "id,username,created_at") and
        layoutIs("SELECT group_concat(name) FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id") and
        layoutIs("SELECT group_concat(name) FROM pragma_index_info('likes_post_id_idx')", "post_id") and
        layoutIs("SELECT group_concat(\"desc\") FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1") and
        layoutIs("SELECT group_concat(\"desc\") FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0");
    if (bt_ok) {
        roots = .{ rootpage("posts"), rootpage("users"), rootpage("posts_created_at_id_idx"), rootpage("likes_post_id_idx") };
        bt_ok = std.mem.indexOfScalar(c_uint, &roots, 0) == null and tds_setup(db, &roots) != 0;
    }
    if (!bt_ok) std.debug.print("unexpected schema: reads use SQL\n", .{});
}

// The log is copied back by a worker thread at low priority, on its own connection, so the loop never waits for an
// fsync. The loop alone decides: it asks for a pass by eventfd and the outcome comes back as a ring completion. A
// log can only restart once all of it is copied, which steady writes prevent, so if frames arrived during a pass the
// loop stops taking POSTs (reads go on) for one more pass. Past WAL_MAX frames it waits for that pass however long it
// takes, because a saturated CPU starves the worker and the log would otherwise grow without bound.
const WAL_START = 8192;
const WAL_MAX = 4 * WAL_START;
const POST_WAIT_NS = 250000000;
const WAL_LEFT = 1; // what a pass reports; a counter value, so never 0
const WAL_ALL = 2;
var wal_db: ?*c.sqlite3 = null;
var wal_asks: i32 = -1;
var wal_frames: c_int = 0;
var wal_asked_at: c_int = 0;
var wal_busy = false;
var wal_last_pass = false;
var post_deadline: i64 = 0; // 0: no limit
fn monoNs() i64 {
    const t = std.posix.clock_gettime(.MONOTONIC) catch unreachable;
    return t.sec * 1000000000 + t.nsec;
}
// WALKICK_MB: once that much new log is written, queue its writeback (SYNC_FILE_RANGE_WRITE on an io-wq thread) so
// dirty WAL pages never pile up to the kernel's dirty threshold, where write() on the loop waits in balance_dirty_pages
// (follows #516). Only starts writeback, never waits: durability is still SQLite's own syncs. 0: off.
const WALKICK_MB = 8;
var kick_fd: i32 = -1;
fn walHook(_: ?*anyopaque, _: ?*c.sqlite3, _: [*c]const u8, frames: c_int) callconv(.c) c_int {
    wal_frames = frames;
    return c.SQLITE_OK;
}
fn walWorker() void {
    _ = linux.syscall3(.setpriority, 0, @intCast(linux.gettid()), 19); // PRIO_PROCESS on this thread
    var go: u64 = 0;
    while (std.c.read(wal_asks, std.mem.asBytes(&go), 8) == 8) {
        var log: c_int = -1;
        var done: c_int = -2;
        const rc = c.sqlite3_wal_checkpoint_v2(wal_db, "main", c.SQLITE_CHECKPOINT_PASSIVE, &log, &done);
        const outcome: u64 = if (rc == c.SQLITE_OK and log == done) WAL_ALL else WAL_LEFT;
        if (std.c.write(wal_replies, std.mem.asBytes(&outcome), 8) != 8) return;
    }
}
fn walAsk() void {
    const one: u64 = 1;
    wal_busy = true;
    wal_asked_at = wal_frames;
    if (std.c.write(wal_asks, std.mem.asBytes(&one), 8) != 8) wal_busy = false;
}
fn walCopied(outcome: u64) void {
    wal_busy = false;
    // All copied, and nothing committed since the pass was asked for: the next write restarts the log.
    if (outcome == WAL_ALL and wal_frames == wal_asked_at) {
        wal_frames = 0;
        wal_last_pass = false;
        hold_posts = false;
        return;
    }
    if (wal_last_pass) { // POSTs were let go early, or the pass failed: try again later
        wal_last_pass = false;
        hold_posts = false;
        return;
    }
    wal_last_pass = true;
    hold_posts = true;
    post_deadline = if (wal_frames >= WAL_MAX) 0 else monoNs() + POST_WAIT_NS;
    walAsk();
}
// Between batches, with no transaction open: a write only restarts the log from a read transaction begun after the
// last copy, so POSTs can't be let go in the middle of a batch.
var kicked_to: i64 = 0;
fn walKick() void {
    if (WALKICK_MB == 0 or kick_fd < 0) return;
    const fin = (32 + @as(i64, wal_frames) * (4096 + 24)) & ~@as(i64, 4095); // whole pages only, the last is still filling
    if (fin < kicked_to) kicked_to = 0; // the log restarted
    if (fin - kicked_to < (@as(i64, WALKICK_MB) << 20)) return;
    const s = sqe(0);
    s.prep_rw(.SYNC_FILE_RANGE, kick_fd, 0, @intCast(fin - kicked_to), @intCast(kicked_to));
    s.rw_flags = 2; // SYNC_FILE_RANGE_WRITE
    s.flags |= linux.IOSQE_ASYNC;
    s.user_data = T_NOP;
    kicked_to = fin;
}
fn walAfterCommit() void {
    walKick();
    if (wal_outcome != 0) {
        const o = wal_outcome;
        wal_outcome = 0;
        walCopied(o);
    }
    if (wal_frames >= WAL_MAX and wal_busy) {
        hold_posts = true;
        post_deadline = 0;
    } else if (!wal_busy and wal_frames >= WAL_START) walAsk();
}
fn walHoldOver() bool {
    if (hold_posts and post_deadline != 0 and monoNs() > post_deadline) hold_posts = false;
    return !hold_posts;
}

// SQLite writes each log frame as two pwrites, its 24-byte header and then the page, so a commit of n pages cost 2n
// syscalls. On the loop thread a commit's frames are gathered here and go out in one pwrite once the commit frame
// (nonzero size field in its header) is complete. SQLite updates the log index only after that, so no reader can
// look for a frame still held here; a read or stat of the same file writes them out first anyway. These replace the
// unix VFS's pwrite, pread and fstat through its documented xSetSystemCall hook.
threadlocal var on_loop = false;
var wbuf: ?[*]u8 = null;
var wlen: usize = 0;
var wcap: usize = 0;
var woff: i64 = 0;
var wfd: c_int = -1;
var wcommit = false;
const PwriteFn = *const fn (c_int, ?*const anyopaque, usize, i64) callconv(.c) isize;
const PreadFn = *const fn (c_int, ?*anyopaque, usize, i64) callconv(.c) isize;
const FstatFn = *const fn (c_int, ?*anyopaque) callconv(.c) c_int;
var sys_pwrite: PwriteFn = undefined;
var sys_pread: PreadFn = undefined;
var sys_fstat: FstatFn = undefined;
fn wflush() bool { // false (errno set) if the write failed
    var done: usize = 0;
    while (done < wlen) {
        const r = sys_pwrite(wfd, wbuf.? + done, wlen - done, woff + @as(i64, @intCast(done)));
        if (r < 0 and std.c._errno().* == @intFromEnum(linux.E.INTR)) continue;
        if (r <= 0) {
            wlen = 0;
            wfd = -1;
            if (r == 0) std.c._errno().* = @intFromEnum(linux.E.IO);
            return false;
        }
        done += @intCast(r);
    }
    wlen = 0;
    wfd = -1;
    return true;
}
fn walPwrite(fd: c_int, p: ?*const anyopaque, n: usize, off: i64) callconv(.c) isize {
    if (!on_loop) return sys_pwrite(fd, p, n, off);
    if (wlen > 0 and (fd != wfd or off != woff + @as(i64, @intCast(wlen))) and !wflush()) return -1;
    if (wlen == 0 and n != 24) return sys_pwrite(fd, p, n, off); // only a frame header starts a run
    if (wlen == 0) {
        wfd = fd;
        woff = off;
    }
    if (wlen + n > wcap) {
        wcap = (wlen + n) * 2;
        wbuf = @ptrCast(realloc(wbuf, wcap) orelse @panic("out of memory"));
    }
    const src: [*]const u8 = @ptrCast(p.?);
    @memcpy(wbuf.?[wlen..][0..n], src[0..n]);
    wlen += n;
    if (n == 24) {
        wcommit = (src[4] | src[5] | src[6] | src[7]) != 0;
    } else if ((wcommit or wlen >= (1 << 20)) and !wflush()) return -1;
    return @intCast(n);
}
fn walPread(fd: c_int, p: ?*anyopaque, n: usize, off: i64) callconv(.c) isize {
    if (wlen > 0 and fd == wfd and on_loop and !wflush()) return -1;
    return sys_pread(fd, p, n, off);
}
fn walFstat(fd: c_int, s: ?*anyopaque) callconv(.c) c_int {
    if (wlen > 0 and fd == wfd and on_loop and !wflush()) return -1;
    return sys_fstat(fd, s);
}

fn dbOpen(path: [:0]const u8) void {
    const v: *c.sqlite3_vfs = c.sqlite3_vfs_find("unix-excl"); // the unix VFSes share one system call table
    on_loop = true;
    if (COMBO) {
        sys_pwrite = @ptrCast(v.xGetSystemCall.?(v, "pwrite").?);
        sys_pread = @ptrCast(v.xGetSystemCall.?(v, "pread").?);
        sys_fstat = @ptrCast(v.xGetSystemCall.?(v, "fstat").?);
        _ = v.xSetSystemCall.?(v, "pwrite", @ptrCast(&walPwrite));
        _ = v.xSetSystemCall.?(v, "pread", @ptrCast(&walPread));
        _ = v.xSetSystemCall.?(v, "fstat", @ptrCast(&walFstat));
    }
    // unix-excl: one process owns the file, the WAL index lives in its heap, and a transaction takes no file locks.
    const fl = c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_NOMUTEX;
    if (c.sqlite3_open_v2(path.ptr, &db, fl, "unix-excl") != c.SQLITE_OK) die("open {s}: {s}", .{ path, std.mem.span(c.sqlite3_errmsg(db)) });
    _ = c.sqlite3_busy_timeout(db, 5000);
    const pragmas = std.fmt.comptimePrint("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=0; PRAGMA cache_size=-{d}; " ++
        "PRAGMA wal_autocheckpoint=0; SELECT count(*) FROM users WHERE id = 1;", .{CACHE_KB});
    if (c.sqlite3_exec(db, pragmas, null, null, null) != c.SQLITE_OK) die("pragma: {s}", .{std.mem.span(c.sqlite3_errmsg(db))});
    _ = c.sqlite3_wal_hook(db, walHook, null);
    if (WALKICK_MB != 0) {
        var w: [4096]u8 = undefined;
        const wp = std.fmt.bufPrintZ(&w, "{s}-wal", .{path}) catch die("path too long", .{});
        kick_fd = std.c.open(wp, .{ .ACCMODE = .RDONLY, .CLOEXEC = true });
    }
    wal_asks = @bitCast(@as(u32, @truncate(linux.eventfd(0, linux.EFD.CLOEXEC))));
    wal_replies = @bitCast(@as(u32, @truncate(linux.eventfd(0, linux.EFD.CLOEXEC | linux.EFD.NONBLOCK))));
    if (wal_asks < 0 or wal_replies < 0 or c.sqlite3_open_v2(path.ptr, &wal_db, fl, "unix-excl") != c.SQLITE_OK or
        c.sqlite3_exec(wal_db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16;", null, null, null) != c.SQLITE_OK)
        die("checkpoint connection failed", .{});
    const t = std.Thread.spawn(.{ .stack_size = 256 << 10 }, walWorker, .{}) catch die("checkpoint thread failed", .{});
    t.detach();
    btreeSetup();
    const cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
    st_feed = prep(cols ++ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(cols ++ "WHERE p.id = ?");
    st_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    st_like = prep(if (bt_ok) "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING" else "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
    st_one = prep("SELECT 1");
    st_begin = prep("BEGIN");
    st_commit = prep("COMMIT");
    st_rollback = prep("ROLLBACK");
}

// Startup shared with the profile trainer (train.zig): HMAC key, page cache, SQLite, database.
pub fn setup(path: [:0]const u8, secret: []const u8) void {
    const sz: usize = @intCast(tds_pagecache(null, 0)); // one contiguous page cache on huge pages, not a malloc per page
    const npg = CACHE_KB / 4;
    _ = tds_pagecache(hugeAlloc(sz * npg), npg);
    _ = c.sqlite3_initialize(); // built with SQLITE_OMIT_AUTOINIT
    b64init();
    hmacKey(secret);
    started = time(null);
    now_s = started;
    arena = hugeAlloc(BATCH_OUT);
    dbOpen(path);
}

pub fn main() !void {
    const path = std.posix.getenv("SQLITE_PATH");
    const secret = std.posix.getenv("JWT_SECRET");
    if (path == null or secret == null) die("SQLITE_PATH and JWT_SECRET are required", .{});
    const host = std.posix.getenv("HOST") orelse "0.0.0.0";
    const port = if (std.posix.getenv("PORT")) |p| std.fmt.parseInt(u16, p, 10) catch die("bad PORT", .{}) else 3000;
    var rl = try std.posix.getrlimit(.NOFILE);
    if (rl.cur < rl.max) {
        rl.cur = rl.max;
        std.posix.setrlimit(.NOFILE, rl) catch {};
    }
    tsize = @intCast(@min(rl.cur, 1 << 20)); // the kernel caps a table at RLIMIT_NOFILE
    setup(path.?, secret.?);

    const addr = std.net.Address.parseIp4(host, port) catch die("bad HOST", .{});
    for (&lfd) |*l| {
        const one = std.mem.toBytes(@as(c_int, 1));
        l.* = @intCast(linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, 0));
        _ = linux.setsockopt(l.*, linux.SOL.SOCKET, linux.SO.REUSEADDR, &one, one.len);
        _ = linux.setsockopt(l.*, linux.SOL.SOCKET, linux.SO.REUSEPORT, &one, one.len);
        _ = linux.setsockopt(l.*, linux.IPPROTO.TCP, linux.TCP.NODELAY, &one, one.len); // accepted sockets inherit it
        if (errno(linux.bind(l.*, &addr.any, addr.getOsSockLen())) != 0 or errno(linux.listen(l.*, 65535)) != 0)
            die("bind/listen {s}:{d} failed", .{ host, port });
    }
    if (!ringInit(0)) die("io_uring setup failed (needs Linux 6.1+ with io_uring enabled)", .{});
    armWalMsg();
    loop();
}

// Profile training (train.zig only): serves a request mix from memory against a synthetic database, so GCC's profile
// of SQLite and src/btree.c sees the server's real hot paths. Never called by the server.
pub fn trainServe(req: []u8) void {
    var closing = false;
    _ = serve(req, &closing);
}
pub fn trainFlush() void {
    nsegs = 0;
    flushBatch();
}
