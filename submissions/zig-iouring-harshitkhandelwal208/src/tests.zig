const std = @import("std");
const server = @import("server.zig");
const dbm = @import("db.zig");
const http = @import("http.zig");
const jwt = @import("jwt.zig");
const bufm = @import("buf.zig");
const jp = @import("jp.zig");

test {
    std.testing.refAllDecls(http);
    std.testing.refAllDecls(jwt);
    std.testing.refAllDecls(bufm);
    std.testing.refAllDecls(jp);
}

test "generation tag 24-bit wraparound" {
    const gen_mask = server.gen_mask;
    try std.testing.expectEqual(@as(u32, 0x00ffffff), gen_mask);

    var gen: u32 = 0x00fffffe;
    gen = (gen +% 1) & gen_mask;
    try std.testing.expectEqual(@as(u32, 0x00ffffff), gen);

    // Wraparound to 0
    gen = (gen +% 1) & gen_mask;
    try std.testing.expectEqual(@as(u32, 0), gen);

    // Verify ud encoding and decoding with gen_mask
    const packed_ud = (@as(u64, 2) << 56) | (@as(u64, 0x00ffffff & gen_mask) << 32) | 12345;
    const decoded_gen: u32 = @truncate((packed_ud >> 32) & gen_mask);
    const decoded_slot: u32 = @truncate(packed_ud);
    try std.testing.expectEqual(@as(u32, 0x00ffffff), decoded_gen);
    try std.testing.expectEqual(@as(u32, 12345), decoded_slot);
}

test "pending buffer limits and rejection" {
    try std.testing.expectEqual(@as(usize, 65535), server.max_pending_size);

    var c = server.Conn{};
    defer {
        if (c.pend_buf) |p| {
            std.heap.page_allocator.free(p[0..c.pend_cap]);
        }
    }

    var dummy_server: server.Server = undefined;

    // Reject remaining larger than max_pending_size
    const huge = try std.testing.allocator.alloc(u8, server.max_pending_size + 1);
    defer std.testing.allocator.free(huge);
    @memset(huge, 'A');

    const ok = dummy_server.savePending(&c, huge);
    try std.testing.expect(!ok);
    try std.testing.expectEqual(@as(u16, 0), c.pend_len);
    try std.testing.expect(c.pend_buf == null);

    // Small remaining succeeds
    const small = "POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345";
    const ok2 = dummy_server.savePending(&c, small);
    try std.testing.expect(ok2);
    try std.testing.expectEqual(@as(u16, @intCast(small.len)), c.pend_len);
    try std.testing.expectEqualStrings(small, c.pend_buf.?[0..c.pend_len]);

    // Clean up
    if (c.pend_buf) |p| {
        bufm.gpa.free(p[0..c.pend_cap]);
        c.pend_buf = null;
        c.pend_cap = 0;
        c.pend_len = 0;
    }
}

test "http parsing pipelined and partial" {
    const raw = "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\nGET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const head1 = http.parseHead(raw);
    switch (head1) {
        .ok => |h| {
            try std.testing.expectEqual(http.Method.get, h.method);
            try std.testing.expectEqualStrings("/feed", h.path);
            const rem = raw[h.head_len..];
            const head2 = http.parseHead(rem);
            switch (head2) {
                .ok => |h2| {
                    try std.testing.expectEqual(http.Method.get, h2.method);
                    try std.testing.expectEqualStrings("/health", h2.path);
                },
                else => try std.testing.expect(false),
            }
        },
        else => try std.testing.expect(false),
    }

    // Incomplete request head returns need_more
    const partial = "GET /fee";
    try std.testing.expect(http.parseHead(partial) == .need_more);
}

test "resp pool acquire and release" {
    var pool = try server.RespPool.init();
    defer pool.deinit();

    const buf1 = pool.acquire();
    try std.testing.expect(buf1 != null);
    try std.testing.expectEqual(@as(usize, server.pool_buf_size), buf1.?.len);

    const buf2 = pool.acquire();
    try std.testing.expect(buf2 != null);

    pool.release(buf1.?.ptr);
    pool.release(buf2.?.ptr);
    try std.testing.expectEqual(@as(usize, server.pool_count), pool.top);
}

test "fifo queue operations" {
    var q = try server.FifoQueue.init(64);
    defer q.deinit();

    try std.testing.expect(q.isEmpty());
    try std.testing.expectEqual(@as(usize, 0), q.count());

    _ = q.push(.{ .slot = 10, .gen = 1, .ring_idx = 0 });
    _ = q.push(.{ .slot = 20, .gen = 2, .ring_idx = 1 });
    try std.testing.expectEqual(@as(usize, 2), q.count());

    const item1 = q.pop();
    try std.testing.expect(item1 != null);
    try std.testing.expectEqual(@as(u32, 10), item1.?.slot);
    try std.testing.expectEqual(@as(u24, 1), item1.?.gen);
    try std.testing.expectEqual(@as(u8, 0), item1.?.ring_idx);

    const item2 = q.pop();
    try std.testing.expect(item2 != null);
    try std.testing.expectEqual(@as(u32, 20), item2.?.slot);
    try std.testing.expectEqual(@as(u24, 2), item2.?.gen);
    try std.testing.expectEqual(@as(u8, 1), item2.?.ring_idx);

    try std.testing.expect(q.isEmpty());
    try std.testing.expect(q.pop() == null);
}

extern "c" fn access(path: [*c]const u8, mode: c_int) c_int;

test "native zig db operations on seed database" {
    if (access("../../seed/feed.db", 0) != 0) return;

    var db: dbm.Db = .{};
    try db.open("../../seed/feed.db");
    defer db.deinit();

    var out = try bufm.Buf.init(65536);
    defer out.deinit();

    // Health
    try std.testing.expect(try db.health(&out, 42));
    try std.testing.expect(out.len > 0);
    out.reset();

    // Check post exists
    try std.testing.expectEqual(dbm.Db.ExistsResult.exists, db.checkPostExists(1));
    try std.testing.expectEqual(dbm.Db.ExistsResult.not_found, db.checkPostExists(999999999));

    // Get post
    const res = db.getPost(1, &out);
    try std.testing.expectEqual(dbm.Db.GetPostResult.ok, res);
    try std.testing.expect(out.len > 0);
    out.reset();

    // Feed
    try db.feed(&out);
    try std.testing.expect(out.len > 0);
    try std.testing.expect(std.mem.startsWith(u8, out.slice(), "{\"posts\":["));

    // Benchmark 1,000 feed queries
    var t_start: std.os.linux.timespec = undefined;
    _ = std.os.linux.clock_gettime(.MONOTONIC, &t_start);
    var n: usize = 0;
    while (n < 1000) : (n += 1) {
        out.reset();
        try db.feed(&out);
    }
    var t_end: std.os.linux.timespec = undefined;
    _ = std.os.linux.clock_gettime(.MONOTONIC, &t_end);
    const ns_total = (@as(i64, t_end.sec) - @as(i64, t_start.sec)) * 1_000_000_000 + (@as(i64, t_end.nsec) - @as(i64, t_start.nsec));
    std.debug.print("\n=== FEED QUERY BENCHMARK: 1,000 queries in {d:.2} ms ({d:.2} us/query) ===\n", .{
        @as(f64, @floatFromInt(ns_total)) / 1_000_000.0,
        @as(f64, @floatFromInt(ns_total)) / 1_000_000.0,
    });

    // Benchmark 1,000 getPost queries
    _ = std.os.linux.clock_gettime(.MONOTONIC, &t_start);
    n = 0;
    while (n < 1000) : (n += 1) {
        out.reset();
        _ = db.getPost(1, &out);
    }
    _ = std.os.linux.clock_gettime(.MONOTONIC, &t_end);
    const ns_post = (@as(i64, t_end.sec) - @as(i64, t_start.sec)) * 1_000_000_000 + (@as(i64, t_end.nsec) - @as(i64, t_start.nsec));
    std.debug.print("=== GET_POST BENCHMARK: 1,000 queries in {d:.2} ms ({d:.2} us/query) ===\n", .{
        @as(f64, @floatFromInt(ns_post)) / 1_000_000.0,
        @as(f64, @floatFromInt(ns_post)) / 1_000_000.0,
    });
}

test "parseId validation" {
    // Valid IDs
    try std.testing.expectEqual(@as(?i64, 1), server.Server.parseId("1"));
    try std.testing.expectEqual(@as(?i64, 42), server.Server.parseId("42"));
    try std.testing.expectEqual(@as(?i64, 250000), server.Server.parseId("250000"));
    try std.testing.expectEqual(@as(?i64, 999999999), server.Server.parseId("999999999"));

    // Invalid IDs
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId(""));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("0"));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("-1"));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("abc"));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("1.5"));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("123a"));
    try std.testing.expectEqual(@as(?i64, null), server.Server.parseId("12345678901234567890")); // > 18 chars
}

test "trimJs whitespace handling" {
    // Standard spaces and tabs
    try std.testing.expectEqualStrings("hello", server.Server.trimJs("   hello   "));
    try std.testing.expectEqualStrings("world", server.Server.trimJs("\t\r\n world \n\t"));

    // Non-breaking space (0xC2, 0xA0)
    const nbsp_str = "\xc2\xa0post content\xc2\xa0";
    try std.testing.expectEqualStrings("post content", server.Server.trimJs(nbsp_str));

    // Zero-width / Unicode spaces and BOM (0xEF, 0xBB, 0xBF)
    const bom_str = "\xef\xbb\xbfspecial\xef\xbb\xbf";
    try std.testing.expectEqualStrings("special", server.Server.trimJs(bom_str));

    // All whitespace string results in empty
    try std.testing.expectEqualStrings("", server.Server.trimJs("   \r\n\t \xc2\xa0 "));
}

test "writeHeaderDirect formatting" {
    var buf: [128]u8 = undefined;

    // 200 OK with keep-alive
    const len200 = server.Server.writeHeaderDirect(&buf, 200, 42, true);
    const s200 = buf[0..len200];
    try std.testing.expect(std.mem.startsWith(u8, s200, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 42\r\n\r\n"));

    // 201 Created with Connection: close
    const len201 = server.Server.writeHeaderDirect(&buf, 201, 100, false);
    const s201 = buf[0..len201];
    try std.testing.expect(std.mem.startsWith(u8, s201, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 100\r\n\r\n"));

    // 404 Not Found
    const len404 = server.Server.writeHeaderDirect(&buf, 404, 0, true);
    const s404 = buf[0..len404];
    try std.testing.expect(std.mem.startsWith(u8, s404, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n"));

    // 500 Internal Server Error
    const len500 = server.Server.writeHeaderDirect(&buf, 500, 15, false);
    const s500 = buf[0..len500];
    try std.testing.expect(std.mem.startsWith(u8, s500, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 15\r\n\r\n"));
}

test "escapedLen and digitsU64" {
    try std.testing.expectEqual(@as(usize, 1), bufm.digitsU64(0));
    try std.testing.expectEqual(@as(usize, 1), bufm.digitsU64(9));
    try std.testing.expectEqual(@as(usize, 2), bufm.digitsU64(10));
    try std.testing.expectEqual(@as(usize, 2), bufm.digitsU64(99));
    try std.testing.expectEqual(@as(usize, 3), bufm.digitsU64(100));
    try std.testing.expectEqual(@as(usize, 4), bufm.digitsU64(9999));

    const s1 = "hello world";
    try std.testing.expectEqual(s1.len, bufm.escapedLen(s1));

    const s2 = "hello \"world\"\n";
    // 2 quotes (+2), 1 newline (+1) -> len + 3
    try std.testing.expectEqual(s2.len + 3, bufm.escapedLen(s2));

    var direct_buf: [128]u8 = undefined;
    const written = bufm.writeJsonStringDirect(&direct_buf, s2);
    try std.testing.expectEqual(bufm.escapedLen(s2), written);

    // SIMD vector boundary and escape tests (>16 bytes, multi-chunk)
    const long_clean = "This is a clean long string without any escapes that exceeds sixteen bytes completely.";
    var long_buf: [256]u8 = undefined;
    const written_clean = bufm.writeJsonStringDirect(&long_buf, long_clean);
    try std.testing.expectEqual(long_clean.len, written_clean);
    try std.testing.expectEqual(long_clean.len, bufm.escapedLen(long_clean));
    try std.testing.expectEqualStrings(long_clean, long_buf[0..written_clean]);

    const long_escaped = "First 16 bytes! \"quoted\" and \\slashed\\ plus \n newline in long string.";
    const written_escaped = bufm.writeJsonStringDirect(&long_buf, long_escaped);
    try std.testing.expectEqual(bufm.escapedLen(long_escaped), written_escaped);

    // Varint fast decoding verification
    const varint_samples = [_]struct { bytes: []const u8, expected: u64 }{
        .{ .bytes = &[_]u8{0x00}, .expected = 0 },
        .{ .bytes = &[_]u8{0x7f}, .expected = 127 },
        .{ .bytes = &[_]u8{ 0x81, 0x00 }, .expected = 128 },
        .{ .bytes = &[_]u8{ 0x81, 0x23 }, .expected = (1 << 7) | 0x23 },
        .{ .bytes = &[_]u8{ 0x82, 0x84, 0x35 }, .expected = (2 << 14) | (4 << 7) | 0x35 },
    };
    for (varint_samples) |tc| {
        var idx: usize = 0;
        const decoded = try dbm.varint(tc.bytes, &idx);
        try std.testing.expectEqual(tc.expected, decoded);
        try std.testing.expectEqual(tc.bytes.len, idx);
    }
}

test "single post serialized into a borrowed response slab" {
    if (access("../../seed/feed.db", 0) != 0) return;

    var db: dbm.Db = .{};
    try db.open("../../seed/feed.db");
    defer db.deinit();

    var direct_dest: [1024]u8 = undefined;
    var buf = bufm.Buf.wrap(direct_dest[http.head_room..]);
    defer buf.deinit();
    try std.testing.expectEqual(dbm.Db.GetPostResult.ok, db.getPost(1, &buf));
    try std.testing.expect(buf.borrowed);
    const out = http.finishResponse(&direct_dest, buf.len, 200, true, false);
    try std.testing.expect(std.mem.startsWith(u8, out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "));
    try std.testing.expect(std.mem.indexOf(u8, out, "\"id\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"body\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"author\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"like_count\":") != null);
    try std.testing.expect(std.mem.endsWith(u8, out, "}}"));

    // Verify Content-Length header matches exact JSON body length
    const sep = "\r\n\r\n";
    const sep_idx = std.mem.indexOf(u8, out, sep).?;
    const body = out[sep_idx + sep.len ..];
    var cl_hdr: [64]u8 = undefined;
    const cl_str = try std.fmt.bufPrint(&cl_hdr, "Content-Length: {}\r\n", .{body.len});
    try std.testing.expect(std.mem.indexOf(u8, out[0 .. sep_idx + 2], cl_str) != null);
}

test "multi-ring dynamic spillover logic" {
    var s: server.Server = undefined;
    s.active_rings = 1;
    for (0..server.k_ring_count) |i| {
        s.rings[i].id = @intCast(i);
        s.rings[i].slots = 1000;
        s.rings[i].margin = 100;
        s.rings[i].nconn = 0;
        s.rings[i].full = false;
        s.rings[i].accepting = false;
        s.rings[i].depth = 64;
        s.rings[i].accepts = @splat(8); // All accepts already armed in this policy-only fixture.
    }

    s.rings[0].accepting = true;

    // While ring 0 has capacity, spillAccepts does not activate ring 1
    s.spillAccepts();
    try std.testing.expectEqual(@as(usize, 1), s.active_rings);
    try std.testing.expect(s.rings[0].accepting);
    try std.testing.expect(!s.rings[1].accepting);

    // Ring 0 reaches threshold and becomes full
    s.rings[0].nconn = 950;
    s.rings[0].full = true;
    s.rings[0].accepting = false;

    // spillAccepts dynamically activates ring 1
    s.spillAccepts();
    try std.testing.expectEqual(@as(usize, 2), s.active_rings);
    try std.testing.expect(s.rings[1].accepting);

    // Ring 1 reaches threshold and becomes full
    s.rings[1].nconn = 950;
    s.rings[1].full = true;
    s.rings[1].accepting = false;

    // spillAccepts dynamically activates ring 2
    s.spillAccepts();
    try std.testing.expectEqual(@as(usize, 3), s.active_rings);
    try std.testing.expect(s.rings[2].accepting);

    // If ring 0 connections drain, it re-arms accepts on ring 0
    s.rings[0].nconn = 200;
    s.rings[0].full = false;
    s.rings[2].full = true;
    s.rings[2].accepting = false;

    s.spillAccepts();
    try std.testing.expect(s.rings[0].accepting);
}
