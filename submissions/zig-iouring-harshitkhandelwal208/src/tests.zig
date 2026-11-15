const std = @import("std");
const server = @import("server.zig");
const dbm = @import("db.zig");
const http = @import("http.zig");
const jwt = @import("jwt.zig");
const bufm = @import("buf.zig");

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

test "sqlite varint decoding" {
    // 1-byte varints (0x00 to 0x7f)
    const p1 = [_]u8{ 0x00, 0x42, 0x7f };
    var i: usize = 0;
    try std.testing.expectEqual(@as(u64, 0), try dbm.varint(&p1, &i));
    try std.testing.expectEqual(@as(u64, 0x42), try dbm.varint(&p1, &i));
    try std.testing.expectEqual(@as(u64, 0x7f), try dbm.varint(&p1, &i));
    try std.testing.expectEqual(@as(usize, 3), i);

    // 2-byte varint (0x81, 0x00 = 128)
    const p2 = [_]u8{ 0x81, 0x00 };
    i = 0;
    try std.testing.expectEqual(@as(u64, 128), try dbm.varint(&p2, &i));
    try std.testing.expectEqual(@as(usize, 2), i);

    // 9-byte varint with full 8 bits in 9th byte
    const p9 = [_]u8{ 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xff };
    i = 0;
    const v9 = try dbm.varint(&p9, &i);
    try std.testing.expectEqual(@as(u64, 0xff), v9);
    try std.testing.expectEqual(@as(usize, 9), i);

    // Truncated buffer returns error
    const p_trunc = [_]u8{ 0x81 };
    i = 0;
    try std.testing.expectError(dbm.DbError.Corrupt, dbm.varint(&p_trunc, &i));
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

test "ring worker rearm and close queues" {
    var qmem: [64]u32 = undefined;
    var cqmem: [64]u32 = undefined;
    var w = server.RingWorker{
        .rearm_queue = &qmem,
        .close_queue = &cqmem,
    };

    w.pushRearm(10);
    w.pushRearm(20);
    try std.testing.expectEqual(@as(?u32, 10), w.popRearm());
    try std.testing.expectEqual(@as(?u32, 20), w.popRearm());
    try std.testing.expectEqual(@as(?u32, null), w.popRearm());

    w.pushClose(42);
    w.pushClose(99);
    try std.testing.expectEqual(@as(?u32, 42), w.popClose());
    try std.testing.expectEqual(@as(?u32, 99), w.popClose());
    try std.testing.expectEqual(@as(?u32, null), w.popClose());
}

extern "c" fn td_count_likes(c: ?*anyopaque, lo: i64, hi: i64, n: [*]u32) c_int;

test "b-tree count likes boundary validation" {
    var counts: [16]u32 = @splat(0);
    // hi < lo must return fallback 1
    const r1 = td_count_likes(null, 100, 50, &counts);
    try std.testing.expectEqual(@as(c_int, 1), r1);

    // hi - lo > 1000 must return fallback 1
    const r2 = td_count_likes(null, 100, 1200, &counts);
    try std.testing.expectEqual(@as(c_int, 1), r2);
}
