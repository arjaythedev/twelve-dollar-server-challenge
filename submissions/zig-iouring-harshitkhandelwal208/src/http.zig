//! HTTP/1.1 request parsing (keep-alive, Content-Length and chunked bodies) and response head assembly.
const std = @import("std");
const Scratch = @import("scratch.zig").Scratch;

pub const max_head = 16 * 1024;
pub const max_body = 8 * 1024 * 1024;
pub const max_chunks = 1024;

pub const Method = enum { get, post, put, delete, other };

pub const Head = struct {
    method: Method,
    path: []const u8, // raw (still percent-encoded), without the query
    query: []const u8, // raw, without '?'
    auth: ?[]const u8 = null,
    content_length: usize = 0,
    chunked: bool = false,
    keep_alive: bool = true,
    gzip: bool = false, // the client lists gzip in Accept-Encoding
    head_len: usize = 0, // bytes up to and including the blank line
};

pub const Parsed = union(enum) {
    need_more,
    bad: u16,
    ok: Head,
};

fn trimOws(s: []const u8) []const u8 {
    var a: usize = 0;
    var b = s.len;
    while (a < b and (s[a] == ' ' or s[a] == '\t')) a += 1;
    while (b > a and (s[b - 1] == ' ' or s[b - 1] == '\t')) b -= 1;
    return s[a..b];
}

/// Position of the next "\r\n" at or after `from`. A vectorised search for the single byte '\r' (the standard
/// library lowers the two-byte search to a byte loop), then a check of the byte after it.
inline fn findCrlf(data: []const u8, from: usize) ?usize {
    var p = from;
    while (std.mem.indexOfScalarPos(u8, data, p, '\r')) |q| {
        if (q + 1 < data.len and data[q + 1] == '\n') return q;
        p = q + 1;
    }
    return null;
}

const V32 = @Vector(32, u8);

/// Bit i set when `block[i] == c`.
inline fn eqMask(block: V32, c: u8) u32 {
    return @bitCast(block == @as(V32, @splat(c)));
}

/// Index of the first `c` in `data[from..to]`, or null. One 32-byte block compare when that many bytes are readable.
inline fn find1(data: []const u8, from: usize, to: usize, c: u8) ?usize {
    if (from + 32 <= data.len) {
        const m = eqMask(data[from..][0..32].*, c);
        if (m != 0) {
            const p = from + @ctz(m);
            return if (p < to) p else null;
        }
        if (from + 32 >= to) return null;
        return std.mem.indexOfScalarPos(u8, data[0..to], from + 32, c);
    }
    return std.mem.indexOfScalarPos(u8, data[0..to], from, c);
}

const max_lines = 128;

/// One pass over the request in 32-byte blocks: finds the "\r\n\r\n" that ends the head (returns its index, or null when
/// it has not arrived) and records the index of every "\r\n" before it, so the header loop does not search again.
fn scanHead(data: []const u8, nl: *[max_lines]u32, nn: *usize) ?usize {
    var i: usize = 0;
    while (i < data.len) : (i += 32) {
        var m: u32 = undefined;
        if (i + 32 <= data.len) {
            m = eqMask(data[i..][0..32].*, '\n');
        } else {
            m = 0;
            for (data[i..], 0..) |ch, k| m |= @as(u32, @intFromBool(ch == '\n')) << @intCast(k);
        }
        while (m != 0) {
            const p = i + @ctz(m);
            m &= m - 1;
            if (p == 0 or data[p - 1] != '\r') continue;
            if (p >= 3 and data[p - 2] == '\n' and data[p - 3] == '\r') return p - 3;
            if (nn.* < max_lines) {
                nl[nn.*] = @intCast(p - 1);
                nn.* += 1;
            }
        }
    }
    return null;
}

/// Whether an Accept-Encoding value lists gzip with a non-zero quality: a comma-separated list of codings, each with
/// optional parameters (`gzip;q=0.5`, `gzip; q=0`, `GZIP ; Q=0.000`).
fn acceptsGzip(val: []const u8) bool {
    var codings = std.mem.splitScalar(u8, val, ',');
    while (codings.next()) |item| {
        var parts = std.mem.splitScalar(u8, item, ';');
        const name = trimOws(parts.next() orelse continue);
        if (!std.ascii.eqlIgnoreCase(name, "gzip")) continue;
        var q_zero = false;
        while (parts.next()) |param| {
            const t = trimOws(param);
            if (t.len >= 2 and (t[0] == 'q' or t[0] == 'Q') and t[1] == '=') {
                const num = trimOws(t[2..]);
                // q = 0, 0., 0.0, 0.00 or 0.000 (at most three decimals) means "not acceptable"
                q_zero = num.len != 0 and num[0] == '0' and
                    (num.len == 1 or (num[1] == '.' and std.mem.indexOfNone(u8, num[2..], "0") == null));
            }
        }
        return !q_zero;
    }
    return false;
}

pub fn parseHead(data: []const u8) Parsed {
    var nl: [max_lines]u32 = undefined;
    var nn: usize = 0;
    var nk: usize = 0; // next recorded line end to look at
    const end = scanHead(data, &nl, &nn) orelse {
        if (data.len > max_head) return .{ .bad = 431 };
        return .need_more;
    };
    if (end + 4 > max_head) return .{ .bad = 431 };
    const head = data[0..end];
    const line_end = if (nn > 0) nl[0] else head.len;
    nk = 1;
    const line = head[0..line_end];
    var method: Method = .other;
    var target: []const u8 = undefined;
    var keep_alive: bool = true;

    var matched = false;
    if (line.len >= 14 and line[line.len - 9] == ' ') {
        const ver_u64 = std.mem.readInt(u64, line[line.len - 8 ..][0..8], .little);
        if (ver_u64 == 0x312e312f50545448 or ver_u64 == 0x302e312f50545448) {
            const w4 = std.mem.readInt(u32, line[0..4], .little);
            if (w4 == 0x20544547) { // "GET "
                method = .get;
                target = line[4 .. line.len - 9];
                keep_alive = (ver_u64 == 0x312e312f50545448);
                matched = true;
            } else if (w4 == 0x54534f50 and line.len >= 15 and line[4] == ' ') { // "POST "
                method = .post;
                target = line[5 .. line.len - 9];
                keep_alive = (ver_u64 == 0x312e312f50545448);
                matched = true;
            }
        }
    }
    if (!matched) {
        const sp1 = find1(data, 0, line_end, ' ') orelse return .{ .bad = 400 };
        const sp2 = std.mem.lastIndexOfScalar(u8, line, ' ') orelse return .{ .bad = 400 };
        if (sp2 <= sp1) return .{ .bad = 400 };
        const m = line[0..sp1];
        target = line[sp1 + 1 .. sp2];
        const ver = line[sp2 + 1 ..];
        method = if (std.mem.eql(u8, m, "GET")) .get else if (std.mem.eql(u8, m, "POST")) .post else if (std.mem.eql(u8, m, "PUT")) .put else if (std.mem.eql(u8, m, "DELETE")) .delete else .other;
        if (std.mem.eql(u8, ver, "HTTP/1.1")) {
            keep_alive = true;
        } else if (std.mem.eql(u8, ver, "HTTP/1.0")) {
            keep_alive = false;
        } else return .{ .bad = 505 };
    }

    var h: Head = .{
        .method = method,
        .path = target,
        .query = "",
        .head_len = end + 4,
        .keep_alive = keep_alive,
    };
    if (target.len == 0 or target[0] != '/') return .{ .bad = 400 };
    if (std.mem.indexOfScalar(u8, target, '?')) |q| {
        h.path = target[0..q];
        h.query = target[q + 1 ..];
    }
    var have_cl = false;
    var pos = line_end;
    while (pos + 2 <= head.len) {
        pos += 2; // skip CRLF
        while (nk < nn and nl[nk] < pos) nk += 1;
        const le: usize = if (nk < nn) nl[nk] else if (nn < max_lines) head.len else (findCrlf(head, pos) orelse head.len);
        const l = head[pos..le];
        pos = le;
        if (l.len == 0) continue;
        const colon_at = find1(data, pos - l.len, le, ':') orelse return .{ .bad = 400 };
        const colon = colon_at - (pos - l.len);
        const name = l[0..colon];
        if (name.len == 0 or name[name.len - 1] == ' ' or name[0] == ' ' or name[0] == '\t') return .{ .bad = 400 };
        const val = trimOws(l[colon + 1 ..]);
        switch (name.len) {
            13 => if (std.ascii.eqlIgnoreCase(name, "authorization")) {
                h.auth = val;
            },
            14 => if (std.ascii.eqlIgnoreCase(name, "content-length")) {
                if (val.len == 0 or val.len > 12) return .{ .bad = 400 };
                var n: usize = 0;
                for (val) |c| {
                    if (c < '0' or c > '9') return .{ .bad = 400 };
                    n = n * 10 + (c - '0');
                }
                if (have_cl and n != h.content_length) return .{ .bad = 400 };
                have_cl = true;
                h.content_length = n;
            },
            10 => if (std.ascii.eqlIgnoreCase(name, "connection")) {
                var it = std.mem.tokenizeAny(u8, val, ", \t");
                while (it.next()) |tok| {
                    if (std.ascii.eqlIgnoreCase(tok, "close")) h.keep_alive = false;
                    if (std.ascii.eqlIgnoreCase(tok, "keep-alive")) h.keep_alive = true;
                }
            },
            15 => if (std.ascii.eqlIgnoreCase(name, "accept-encoding")) {
                h.gzip = acceptsGzip(val);
            },
            17 => if (std.ascii.eqlIgnoreCase(name, "transfer-encoding")) {
                if (std.ascii.eqlIgnoreCase(val, "chunked")) h.chunked = true else return .{ .bad = 501 };
            },
            else => {},
        }
    }
    if (h.chunked and have_cl) return .{ .bad = 400 };
    if (h.content_length > max_body) return .{ .bad = 413 };
    return .{ .ok = h };
}

pub const Body = union(enum) {
    need_more,
    bad: u16,
    /// `data` is the body; `consumed` is the number of bytes after the head that belong to this request.
    ok: struct { data: []const u8, consumed: usize },
};

/// `rest` is everything buffered after the head.
pub fn takeBody(h: *const Head, rest: []const u8, sc: *Scratch) Body {
    if (!h.chunked) {
        if (rest.len < h.content_length) return .need_more;
        return .{ .ok = .{ .data = rest[0..h.content_length], .consumed = h.content_length } };
    }
    // Chunked: validate the framing first (sizes), then copy the payload into scratch memory.
    // Framing is re-checked from the start whenever more bytes arrive, so everything that is scanned is bounded: at most
    // `max_chunks` chunks, size lines of at most 64 bytes (extensions included) and 4 KiB of trailers. Payload bytes are
    // jumped over by their size, never scanned.
    var pos: usize = 0;
    var total: usize = 0;
    var n_chunks: usize = 0;
    while (true) {
        const le = findCrlf(rest, pos) orelse return if (rest.len - pos > 64) .{ .bad = 400 } else .need_more;
        if (le - pos > 64) return .{ .bad = 400 };
        var size_s = rest[pos..le];
        if (std.mem.indexOfScalar(u8, size_s, ';')) |sc_i| size_s = size_s[0..sc_i];
        size_s = trimOws(size_s);
        if (size_s.len == 0 or size_s.len > 8) return .{ .bad = 400 };
        const size = std.fmt.parseInt(usize, size_s, 16) catch return .{ .bad = 400 };
        pos = le + 2;
        if (size == 0) {
            // trailers until an empty line
            const trailers_at = pos;
            while (true) {
                if (pos - trailers_at > 4096) return .{ .bad = 400 };
                const te = findCrlf(rest, pos) orelse return if (rest.len - pos > 4096) .{ .bad = 400 } else .need_more;
                const empty = te == pos;
                pos = te + 2;
                if (empty) break;
            }
            break;
        }
        total += size;
        n_chunks += 1;
        if (n_chunks > max_chunks) return .{ .bad = 413 };
        if (total > max_body) return .{ .bad = 413 };
        if (pos + size + 2 > rest.len) return .need_more;
        if (rest[pos + size] != '\r' or rest[pos + size + 1] != '\n') return .{ .bad = 400 };
        pos += size + 2;
    }
    const out = sc.alloc(total) catch return .{ .bad = 500 };
    var w: usize = 0;
    var p: usize = 0;
    while (true) {
        const le = findCrlf(rest, p).?;
        var size_s = rest[p..le];
        if (std.mem.indexOfScalar(u8, size_s, ';')) |sc_i| size_s = size_s[0..sc_i];
        const size = std.fmt.parseInt(usize, trimOws(size_s), 16) catch unreachable;
        p = le + 2;
        if (size == 0) break;
        @memcpy(out[w .. w + size], rest[p .. p + size]);
        w += size;
        p += size + 2;
    }
    return .{ .ok = .{ .data = out, .consumed = pos } };
}

pub fn statusLine(code: u16) []const u8 {
    return switch (code) {
        200 => "HTTP/1.1 200 OK\r\n",
        201 => "HTTP/1.1 201 Created\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n",
        401 => "HTTP/1.1 401 Unauthorized\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n",
        413 => "HTTP/1.1 413 Content Too Large\r\n",
        431 => "HTTP/1.1 431 Request Header Fields Too Large\r\n",
        500 => "HTTP/1.1 500 Internal Server Error\r\n",
        501 => "HTTP/1.1 501 Not Implemented\r\n",
        503 => "HTTP/1.1 503 Service Unavailable\r\n",
        505 => "HTTP/1.1 505 HTTP Version Not Supported\r\n",
        else => "HTTP/1.1 500 Internal Server Error\r\n",
    };
}

pub const head_room = 160;

/// The response is assembled in one buffer: `head_room` bytes reserved in front of the body, and the head is
/// written right-aligned just before the body so the whole response is one contiguous slice.
/// `buf[head_room..]` holds the body on entry. Returns the slice to send.
pub fn finishResponse(buf: []u8, body_len: usize, code: u16, keep_alive: bool, gzip: bool) []u8 {
    var tmp: [head_room]u8 = undefined;
    var n: usize = 0;
    const sl = statusLine(code);
    @memcpy(tmp[n..][0..sl.len], sl);
    n += sl.len;
    const ct = "Content-Type: application/json\r\n";
    @memcpy(tmp[n..][0..ct.len], ct);
    n += ct.len;
    const cl = "Content-Length: ";
    @memcpy(tmp[n..][0..cl.len], cl);
    n += cl.len;
    var digits: [20]u8 = undefined;
    var d: usize = digits.len;
    var v = body_len;
    while (true) {
        d -= 1;
        digits[d] = '0' + @as(u8, @intCast(v % 10));
        v /= 10;
        if (v == 0) break;
    }
    @memcpy(tmp[n..][0 .. digits.len - d], digits[d..]);
    n += digits.len - d;
    tmp[n..][0..2].* = "\r\n".*;
    n += 2;
    if (gzip) {
        const ce = "Content-Encoding: gzip\r\n";
        @memcpy(tmp[n..][0..ce.len], ce);
        n += ce.len;
    }
    if (!keep_alive) {
        const c = "Connection: close\r\n";
        @memcpy(tmp[n..][0..c.len], c);
        n += c.len;
    }
    tmp[n] = '\r';
    tmp[n + 1] = '\n';
    n += 2;
    const start = head_room - n;
    @memcpy(buf[start..head_room], tmp[0..n]);
    return buf[start .. head_room + body_len];
}

test "parse head" {
    const req = "GET /api/articles?limit=5&offset=0 HTTP/1.1\r\nHost: x\r\nAuthorization: Token abc.def.ghi\r\nContent-Length: 0\r\n\r\n";
    const p = parseHead(req);
    try std.testing.expect(p == .ok);
    try std.testing.expectEqualStrings("/api/articles", p.ok.path);
    try std.testing.expectEqualStrings("limit=5&offset=0", p.ok.query);
    try std.testing.expectEqualStrings("Token abc.def.ghi", p.ok.auth.?);
    try std.testing.expect(parseHead("GET / HTTP/1.1\r\nHost: x\r\n") == .need_more);
    try std.testing.expectEqual(@as(u16, 400), parseHead("GET\r\n\r\n").bad);
}

test "chunked" {
    var sc = try Scratch.init(1024);
    const h = Head{ .method = .post, .path = "/", .query = "", .chunked = true };
    const r = takeBody(&h, "5\r\nhello\r\n6;x=1\r\n world\r\n0\r\n\r\nNEXT", &sc);
    try std.testing.expect(r == .ok);
    try std.testing.expectEqualStrings("hello world", r.ok.data);
    try std.testing.expectEqual(@as(usize, 30), r.ok.consumed);
    try std.testing.expect(takeBody(&h, "5\r\nhel", &sc) == .need_more);
}

fn headEnd(data: []const u8) ?usize {
    var nl: [max_lines]u32 = undefined;
    var nn: usize = 0;
    return scanHead(data, &nl, &nn);
}

test "head end and crlf search" {
    try std.testing.expectEqual(@as(?usize, 14), headEnd("GET / HTTP/1.1\r\n\r\nBODY"));
    try std.testing.expectEqual(@as(?usize, null), headEnd("GET / HTTP/1.1\r\nHost: x\r\n"));
    try std.testing.expectEqual(@as(?usize, null), headEnd("a\r\n\r"));
    try std.testing.expectEqual(@as(?usize, 1), headEnd("a\r\n\r\n"));
    try std.testing.expectEqual(@as(?usize, 2), headEnd("a\r\r\n\r\n"));
    try std.testing.expectEqual(@as(?usize, null), headEnd("a\n\n\r\r"));
    try std.testing.expectEqual(@as(?usize, 4), findCrlf("ab\rc\r\nx", 0));
    try std.testing.expectEqual(@as(?usize, null), findCrlf("abc\r", 0));
}

test "head scan agrees with a plain search on random heads" {
    var prng = std.Random.DefaultPrng.init(99);
    const r = prng.random();
    var buf: [400]u8 = undefined;
    var k: usize = 0;
    while (k < 100_000) : (k += 1) {
        const n = r.uintLessThan(usize, buf.len);
        const alphabet = "ab:\r\n \r\n\r\n";
        for (buf[0..n]) |*c| c.* = alphabet[r.uintLessThan(usize, alphabet.len)];
        const want = std.mem.indexOf(u8, buf[0..n], "\r\n\r\n");
        var nl: [max_lines]u32 = undefined;
        var nn: usize = 0;
        try std.testing.expectEqual(want, scanHead(buf[0..n], &nl, &nn));
        if (want) |end| {
            // recorded line ends are exactly the "\r\n" positions up to and including the end of the head
            var at: usize = 0;
            var j: usize = 0;
            while (std.mem.indexOfPos(u8, buf[0..n], at, "\r\n")) |q| : (at = q + 1) {
                if (q > end) break;
                if (j < max_lines) {
                    try std.testing.expect(j < nn);
                    try std.testing.expectEqual(@as(u32, @intCast(q)), nl[j]);
                }
                j += 1;
            }
        }
    }
}

test "response head" {
    var buf: [head_room + 1234]u8 = undefined;
    @memset(buf[head_room..], 'x');
    const r = finishResponse(&buf, 1234, 200, true, true);
    try std.testing.expect(std.mem.startsWith(u8, r, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1234\r\nContent-Encoding: gzip\r\n\r\nxxx"));
    const r0 = finishResponse(&buf, 0, 404, false, false);
    try std.testing.expectEqualStrings("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", r0);
}

test "accept-encoding" {
    try std.testing.expect(acceptsGzip("gzip"));
    try std.testing.expect(acceptsGzip("gzip, deflate, br"));
    try std.testing.expect(acceptsGzip("deflate, GZIP"));
    try std.testing.expect(acceptsGzip("br;q=1.0, gzip;q=0.8"));
    try std.testing.expect(acceptsGzip("gzip ; q=0.001"));
    try std.testing.expect(!acceptsGzip(""));
    try std.testing.expect(!acceptsGzip("identity"));
    try std.testing.expect(!acceptsGzip("gzip;q=0"));
    try std.testing.expect(!acceptsGzip("gzip; q=0"));
    try std.testing.expect(!acceptsGzip("gzip;q=0.0"));
    try std.testing.expect(!acceptsGzip("GZIP ; Q=0.000, identity"));
    try std.testing.expect(!acceptsGzip("deflate, gzip;q=0 , br"));
    try std.testing.expect(!acceptsGzip("xgzip, gzipx"));
}

test "too many chunks" {
    var sc = try Scratch.init(1 << 16);
    const h = Head{ .method = .post, .path = "/", .query = "", .chunked = true };
    var body: [8 * (max_chunks + 2)]u8 = undefined;
    var n: usize = 0;
    for (0..max_chunks + 1) |_| {
        @memcpy(body[n..][0..8], "1\r\nX\r\n\x00\x00");
        n += 6;
    }
    try std.testing.expectEqual(@as(u16, 413), takeBody(&h, body[0..n], &sc).bad);
}

test "chunk framing is bounded" {
    var sc = try Scratch.init(1 << 16);
    const h = Head{ .method = .post, .path = "/", .query = "", .chunked = true };
    // many short trailer lines without the closing blank line: refused once past 4 KiB instead of rescanned forever
    var buf: [10000]u8 = undefined;
    var n: usize = 0;
    @memcpy(buf[0..3], "0\r\n");
    n = 3;
    while (n + 6 < buf.len) : (n += 5) @memcpy(buf[n..][0..5], "a:b\r\n");
    try std.testing.expectEqual(@as(u16, 400), takeBody(&h, buf[0..n], &sc).bad);
    // a size line with a long extension is refused
    var ext: [200]u8 = undefined;
    @memcpy(ext[0..4], "5;x=");
    @memset(ext[4..190], 'y');
    @memcpy(ext[190..194], "\r\nab");
    try std.testing.expectEqual(@as(u16, 400), takeBody(&h, ext[0..194], &sc).bad);
    // ordinary framing still parses, with a short trailer
    const ok = takeBody(&h, "5;name=v\r\nhello\r\n0\r\nX-T: 1\r\n\r\nNEXT", &sc);
    try std.testing.expectEqualStrings("hello", ok.ok.data);
}
