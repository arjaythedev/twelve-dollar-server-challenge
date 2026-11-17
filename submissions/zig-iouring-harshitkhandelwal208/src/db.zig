//! High-speed SQLite database engine over direct B-tree cursors and prepared statements.
//! Bypasses SQL bytecode VM for /feed and /posts/:id, achieving ~2-3 us query latency.
const std = @import("std");
const bufm = @import("buf.zig");
const Buf = bufm.Buf;

pub const c = @import("c");

const POSTS = 0;
const USERS = 1;
const FEED_IDX = 2;
const LIKES_IDX = 3;
const FEED_LEN = 20;
const RANGE_SPAN = 512;

extern "c" fn td_open(path: [*c]const u8, err: [*c]u8, n: c_int) c_int;
extern "c" fn td_begin() c_int;
extern "c" fn td_commit() c_int;
extern "c" fn td_rollback() void;
extern "c" fn td_tick() void;
extern "c" fn td_writes_held() c_int;
extern "c" fn td_in_batch() c_int;
extern "c" fn td_cursor(i: c_int) ?*anyopaque;
extern "c" fn td_seek(c: ?*anyopaque, id: i64) c_int;
extern "c" fn td_seek_ge(c: ?*anyopaque, k: i64) c_int;
extern "c" fn td_first(c: ?*anyopaque) c_int;
extern "c" fn td_next(c: ?*anyopaque) c_int;
extern "c" fn td_payload(c: ?*anyopaque, len: *u32) ?[*]const u8;
extern "c" fn td_rowid(c: ?*anyopaque) i64;
extern "c" fn td_count_likes(c: ?*anyopaque, lo: i64, hi: i64, n: [*]u32) c_int;
extern "c" fn td_insert_post(uid: i64, body: [*]const u8, len: c_int, id: *i64, created: [*]u8, clen: *c_int) c_int;
extern "c" fn td_insert_like(uid: i64, pid: i64) c_int;
extern "c" fn td_delete_like(uid: i64, pid: i64) c_int;
extern "c" fn td_health(err: [*c]u8, n: c_int) c_int;
extern "c" fn td_feed_sql_step(
    id: *i64,
    body: *[*]const u8,
    blen: *c_int,
    ca: *[*]const u8,
    clen: *c_int,
    author: *[*]const u8,
    alen: *c_int,
    likes: *i64,
) c_int;
extern "c" fn td_feed_sql_reset() void;
extern "c" fn td_close() void;

pub const DbError = error{
    Corrupt,
    NotFound,
    DbFailed,
    InsertFailed,
    OpenFailed,
    PathTooLong,
};

pub inline fn varint(p: []const u8, i: *usize) !u64 {
    if (i.* >= p.len) return DbError.Corrupt;
    const b0 = p[i.*];
    if (b0 < 0x80) {
        i.* += 1;
        return b0;
    }
    return varintLong(p, i);
}

pub inline fn varintLong(p: []const u8, i: *usize) !u64 {
    const b0 = p[i.*];
    if (i.* + 1 < p.len) {
        const b1 = p[i.* + 1];
        if (b1 < 0x80) {
            i.* += 2;
            return (@as(u64, b0 & 0x7f) << 7) | b1;
        }
        if (i.* + 2 < p.len) {
            const b2 = p[i.* + 2];
            if (b2 < 0x80) {
                i.* += 3;
                return (@as(u64, b0 & 0x7f) << 14) | (@as(u64, b1 & 0x7f) << 7) | b2;
            }
        }
    }
    var v: u64 = 0;
    var k: usize = 0;
    while (k < 9) : (k += 1) {
        if (i.* >= p.len) return DbError.Corrupt;
        const b = p[i.*];
        i.* += 1;
        if (k == 8) {
            return (v << 8) | b;
        }
        v = (v << 7) | (b & 0x7f);
        if (b & 0x80 == 0) return v;
    }
    return v;
}

const serial_lens = [12]u8{ 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0 };

inline fn serialLen(t: u64) usize {
    if (t < 12) return serial_lens[t];
    return @intCast((t - 12) >> 1);
}

inline fn intOf(t: u64, b: []const u8) !i64 {
    return switch (t) {
        8 => 0,
        9 => 1,
        1 => @as(i8, @bitCast(b[0])),
        2 => std.mem.readInt(i16, b[0..2], .big),
        3 => {
            const raw = (@as(u32, b[0]) << 24) | (@as(u32, b[1]) << 16) | (@as(u32, b[2]) << 8);
            return @as(i32, @bitCast(raw)) >> 8;
        },
        4 => std.mem.readInt(i32, b[0..4], .big),
        5 => {
            const raw = (@as(u64, b[0]) << 56) | (@as(u64, b[1]) << 48) | (@as(u64, b[2]) << 40) |
                (@as(u64, b[3]) << 32) | (@as(u64, b[4]) << 24) | (@as(u64, b[5]) << 16);
            return @as(i64, @bitCast(raw)) >> 16;
        },
        6 => std.mem.readInt(i64, b[0..8], .big),
        else => DbError.Corrupt,
    };
}

const Col = struct {
    t: u64,
    b: []const u8,
};

fn columns(comptime N: usize, rec: []const u8) ![N]Col {
    var i: usize = 0;
    const hsize = try varint(rec, &i);
    if (hsize > rec.len) return DbError.Corrupt;
    var body = hsize;
    var cols: [N]Col = undefined;
    for (0..N) |k| {
        if (i >= hsize) return DbError.Corrupt;
        const t = try varint(rec[0..hsize], &i);
        const len = serialLen(t);
        if (body + len > rec.len) return DbError.Corrupt;
        cols[k] = .{
            .t = t,
            .b = rec[body .. body + len],
        };
        body += len;
    }
    return cols;
}

inline fn text(col: Col) ![]const u8 {
    if (col.t < 13 or (col.t & 1) == 0) return DbError.Corrupt;
    return col.b;
}

inline fn getPayload(cur: ?*anyopaque) ![]const u8 {
    var len: u32 = 0;
    const ptr = td_payload(cur, &len) orelse return DbError.DbFailed;
    return ptr[0..len];
}

const Row = struct {
    id: i64 = 0,
    user_id: i64 = 0,
    body: []const u8 = "",
    created: []const u8 = "",
    author: []const u8 = "",
    likes: i64 = 0,
};

pub const Db = struct {
    pub const ExistsResult = enum { exists, not_found, err };
    pub const GetPostResult = enum { ok, not_found, err };
    pub const GetPostDirectOk = struct {
        start_off: usize,
        total_len: usize,
    };
    pub const GetPostDirectResult = union(enum) {
        ok: GetPostDirectOk,
        not_found,
        err,
    };
    pub const LikeResult = enum { created, already_liked, not_found, err };
    pub const UnlikeResult = enum { ok, not_found, err };

    in_tx: bool = false,

    pub fn open(self: *Db, path: []const u8) !void {
        self.* = .{};
        var path_z: [512:0]u8 = undefined;
        if (path.len >= 512) return error.PathTooLong;
        @memcpy(path_z[0..path.len], path);
        path_z[path.len] = 0;

        var err: [256]u8 = undefined;
        if (td_open(&path_z, &err, 256) != 0) {
            std.log.err("td_open failed: {s}", .{err});
            return error.OpenFailed;
        }
    }

    pub fn init(path: []const u8) !Db {
        var self: Db = .{};
        try self.open(path);
        return self;
    }

    pub fn deinit(self: *Db) void {
        _ = self;
        td_close();
    }

    pub fn beginTx(self: *Db) bool {
        self.in_tx = true;
        return td_begin() == 0;
    }

    pub fn commitTx(self: *Db) bool {
        if (!self.in_tx) return true;
        self.in_tx = false;
        return td_commit() == 0;
    }

    pub fn rollbackTx(self: *Db) void {
        self.in_tx = false;
        td_rollback();
    }

    pub inline fn tick(self: *Db) void {
        _ = self;
        td_tick();
    }

    pub inline fn writesHeld(self: *Db) bool {
        _ = self;
        return td_writes_held() != 0;
    }

    pub inline fn inBatch(self: *Db) bool {
        _ = self;
        return td_in_batch() != 0;
    }

    fn countLikes(cur: ?*anyopaque, id: i64) !i64 {
        var n: u32 = 0;
        const res = td_count_likes(cur, id, id, (&n)[0..1]);
        if (res == 0) return @intCast(n);
        if (res == 1) {
            var r = td_seek_ge(cur, id);
            var count: i64 = 0;
            while (r == 1) {
                const p = try getPayload(cur);
                const cols = try columns(1, p);
                const pid = try intOf(cols[0].t, cols[0].b);
                if (pid != id) break;
                count += 1;
                r = td_next(cur);
            }
            if (r < 0) return DbError.DbFailed;
            return count;
        }
        return DbError.DbFailed;
    }

    pub fn feed(self: *Db, out: *Buf) !void {
        if (!self.beginTx()) return self.feedSql(out);

        const cp = td_cursor(POSTS);
        const cu = td_cursor(USERS);
        const ci = td_cursor(FEED_IDX);
        const cl = td_cursor(LIKES_IDX);

        var rows: [FEED_LEN]Row = undefined;
        var n_rows: usize = 0;

        // 1. Walk feed index: newest first (created_at DESC, id DESC)
        var r = td_first(ci);
        while (r == 1 and n_rows < FEED_LEN) {
            const p = getPayload(ci) catch return self.feedSql(out);
            const cols = columns(3, p) catch return self.feedSql(out);
            const post_id = intOf(cols[1].t, cols[1].b) catch return self.feedSql(out);
            const created = text(cols[0]) catch return self.feedSql(out);
            rows[n_rows] = .{
                .id = post_id,
                .created = created,
            };
            n_rows += 1;
            r = td_next(ci);
        }
        if (r < 0) return self.feedSql(out);

        if (n_rows == 0) {
            out.writeAll("{\"posts\":[]}");
            return;
        }

        // 2. Sort order array in ascending post id order using branch-friendly insertion sort (n_rows <= 20)
        var order: [FEED_LEN]usize = undefined;
        for (0..n_rows) |i| order[i] = i;
        var s_i: usize = 1;
        while (s_i < n_rows) : (s_i += 1) {
            const key = order[s_i];
            const key_id = rows[key].id;
            var s_j: usize = s_i;
            while (s_j > 0 and rows[order[s_j - 1]].id > key_id) : (s_j -= 1) {
                order[s_j] = order[s_j - 1];
            }
            order[s_j] = key;
        }

        const min_id: i64 = rows[order[0]].id;
        const max_id: i64 = rows[order[n_rows - 1]].id;

        for (0..n_rows) |k| {
            const idx = order[k];
            const pid = rows[idx].id;
            if (td_seek(cp, pid) != 1) return self.feedSql(out);
            const p = getPayload(cp) catch return self.feedSql(out);
            const cols = columns(3, p) catch return self.feedSql(out);
            rows[idx].user_id = intOf(cols[1].t, cols[1].b) catch return self.feedSql(out);
            rows[idx].body = text(cols[2]) catch return self.feedSql(out);
        }

        // 3. Fetch authors by rowid seek (caching recent authors in 128-entry direct table)
        var author_cache: [128]struct { uid: i64 = -1, author: []const u8 = "" } = @splat(.{});
        for (0..n_rows) |i| {
            const uid = rows[i].user_id;
            const slot = @as(usize, @intCast(uid ^ (uid >> 6))) & 127;
            if (author_cache[slot].uid == uid) {
                rows[i].author = author_cache[slot].author;
            } else {
                if (td_seek(cu, uid) != 1) return self.feedSql(out);
                const p = getPayload(cu) catch return self.feedSql(out);
                const cols = columns(2, p) catch return self.feedSql(out);
                const a = text(cols[1]) catch return self.feedSql(out);
                rows[i].author = a;
                author_cache[slot] = .{ .uid = uid, .author = a };
            }
        }

        // 4. Like counts: range walk when span <= RANGE_SPAN
        if (max_id >= min_id and max_id - min_id <= RANGE_SPAN) {
            const span: usize = @intCast(max_id - min_id + 1);
            var counts_buf: [RANGE_SPAN + 1]u32 = @splat(0);
            if (td_count_likes(cl, min_id, max_id, &counts_buf) == 0) {
                for (0..n_rows) |i| {
                    const idx: usize = @intCast(rows[i].id - min_id);
                    if (idx < span) rows[i].likes = counts_buf[idx];
                }
            } else {
                for (0..n_rows) |i| {
                    rows[i].likes = countLikes(cl, rows[i].id) catch return self.feedSql(out);
                }
            }
        } else {
            for (0..n_rows) |i| {
                rows[i].likes = countLikes(cl, rows[i].id) catch return self.feedSql(out);
            }
        }

        // 5. Emit JSON directly into output buffer with single ensure
        try out.ensure(n_rows * 650 + 64);
        var ptr = out.ptr + out.len;
        const initial_ptr = ptr;

        @memcpy(ptr[0..10], "{\"posts\":[");
        ptr += 10;

        for (0..n_rows) |i| {
            if (i > 0) {
                ptr[0] = ',';
                ptr += 1;
            }
            @memcpy(ptr[0..6], "{\"id\":");
            ptr += 6;
            ptr += bufm.writeU64Direct(ptr[0..24], @intCast(rows[i].id));

            @memcpy(ptr[0..9], ",\"body\":\"");
            ptr += 9;
            ptr += bufm.writeJsonStringDirect(ptr, rows[i].body);

            @memcpy(ptr[0..16], "\",\"created_at\":\"");
            ptr += 16;
            @memcpy(ptr[0..rows[i].created.len], rows[i].created);
            ptr += rows[i].created.len;

            @memcpy(ptr[0..12], "\",\"author\":\"");
            ptr += 12;
            ptr += bufm.writeJsonStringDirect(ptr, rows[i].author);

            @memcpy(ptr[0..15], "\",\"like_count\":");
            ptr += 15;
            ptr += bufm.writeU64Direct(ptr[0..24], @intCast(rows[i].likes));

            ptr[0] = '}';
            ptr += 1;
        }

        @memcpy(ptr[0..2], "]}");
        ptr += 2;

        out.len += @intFromPtr(ptr) - @intFromPtr(initial_ptr);
    }

    pub fn feedSql(self: *Db, out: *Buf) !void {
        _ = self;
        td_feed_sql_reset();
        out.writeAll("{\"posts\":[");
        var first = true;
        while (true) {
            var id: i64 = 0;
            var body_ptr: [*]const u8 = undefined;
            var body_len: c_int = 0;
            var ca_ptr: [*]const u8 = undefined;
            var ca_len: c_int = 0;
            var author_ptr: [*]const u8 = undefined;
            var author_len: c_int = 0;
            var likes: i64 = 0;

            const rc = td_feed_sql_step(&id, &body_ptr, &body_len, &ca_ptr, &ca_len, &author_ptr, &author_len, &likes);
            if (rc == 0) break;
            if (rc < 0) return DbError.DbFailed;

            if (!first) out.writeByte(',') else first = false;
            out.writeAll("{\"id\":");
            out.writeU64(@intCast(id));
            out.writeAll(",\"body\":\"");
            out.writeJsonString(body_ptr[0..@intCast(body_len)]);
            out.writeAll("\",\"created_at\":\"");
            out.writeAll(ca_ptr[0..@intCast(ca_len)]);
            out.writeAll("\",\"author\":\"");
            out.writeJsonString(author_ptr[0..@intCast(author_len)]);
            out.writeAll("\",\"like_count\":");
            out.writeU64(@intCast(likes));
            out.writeByte('}');
        }
        out.writeAll("]}");
    }

    pub fn getPost(self: *Db, id: i64, out: *Buf) GetPostResult {
        if (!self.beginTx()) return .err;
        const cp = td_cursor(POSTS);
        const cu = td_cursor(USERS);
        const cl = td_cursor(LIKES_IDX);

        const seek_res = td_seek(cp, id);
        if (seek_res == 0) return .not_found;
        if (seek_res < 0) return .err;

        const p = getPayload(cp) catch return .err;
        const cols = columns(4, p) catch return .err;
        const uid = intOf(cols[1].t, cols[1].b) catch return .err;
        const body = text(cols[2]) catch return .err;
        const created = text(cols[3]) catch return .err;

        if (td_seek(cu, uid) != 1) return .not_found;
        const up = getPayload(cu) catch return .err;
        const ucols = columns(2, up) catch return .err;
        const author = text(ucols[1]) catch return .err;

        const likes = countLikes(cl, id) catch return .err;

        out.ensure(body.len * 6 + author.len * 6 + 128) catch return .err;
        var ptr = out.ptr + out.len;
        const initial_ptr = ptr;

        @memcpy(ptr[0..14], "{\"post\":{\"id\":");
        ptr += 14;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(id));

        @memcpy(ptr[0..9], ",\"body\":\"");
        ptr += 9;
        ptr += bufm.writeJsonStringDirect(ptr, body);

        @memcpy(ptr[0..16], "\",\"created_at\":\"");
        ptr += 16;
        @memcpy(ptr[0..created.len], created);
        ptr += created.len;

        @memcpy(ptr[0..12], "\",\"author\":\"");
        ptr += 12;
        ptr += bufm.writeJsonStringDirect(ptr, author);

        @memcpy(ptr[0..15], "\",\"like_count\":");
        ptr += 15;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(likes));

        @memcpy(ptr[0..2], "}}");
        ptr += 2;

        out.len += @intFromPtr(ptr) - @intFromPtr(initial_ptr);
        return .ok;
    }

    pub fn getPostDirect(self: *Db, id: i64, dest: [*]u8, keep_alive: bool) GetPostDirectResult {
        if (!self.beginTx()) return .err;
        const cp = td_cursor(POSTS);
        const cu = td_cursor(USERS);
        const cl = td_cursor(LIKES_IDX);

        const seek_res = td_seek(cp, id);
        if (seek_res == 0) return .not_found;
        if (seek_res < 0) return .err;

        const p = getPayload(cp) catch return .err;
        const cols = columns(4, p) catch return .err;
        const uid = intOf(cols[1].t, cols[1].b) catch return .err;
        const body = text(cols[2]) catch return .err;
        const created = text(cols[3]) catch return .err;

        if (td_seek(cu, uid) != 1) return .not_found;
        const up = getPayload(cu) catch return .err;
        const ucols = columns(2, up) catch return .err;
        const author = text(ucols[1]) catch return .err;

        const likes = countLikes(cl, id) catch return .err;

        // Serialize JSON body starting at dest + 128 in a single linear pass (no redundant escaping pre-scan)
        const body_start = dest + 128;
        var ptr = body_start;

        @memcpy(ptr[0..14], "{\"post\":{\"id\":");
        ptr += 14;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(id));

        @memcpy(ptr[0..9], ",\"body\":\"");
        ptr += 9;
        ptr += bufm.writeJsonStringDirect(ptr, body);

        @memcpy(ptr[0..16], "\",\"created_at\":\"");
        ptr += 16;
        @memcpy(ptr[0..created.len], created);
        ptr += created.len;

        @memcpy(ptr[0..12], "\",\"author\":\"");
        ptr += 12;
        ptr += bufm.writeJsonStringDirect(ptr, author);

        @memcpy(ptr[0..15], "\",\"like_count\":");
        ptr += 15;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(likes));

        @memcpy(ptr[0..2], "}}");
        ptr += 2;

        const body_len = @intFromPtr(ptr) - @intFromPtr(body_start);

        // Prepend HTTP response headers directly into the 128-byte headroom before dest + 128
        const prefix = if (keep_alive)
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        else
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";
        const d_len = bufm.digitsU64(body_len);
        const hdr_len = prefix.len + d_len + 4;
        const start_off = 128 - hdr_len;
        var h_ptr = dest + start_off;
        @memcpy(h_ptr[0..prefix.len], prefix);
        h_ptr += prefix.len;
        _ = bufm.writeU64Direct(h_ptr[0..24], body_len);
        h_ptr += d_len;
        @memcpy(h_ptr[0..4], "\r\n\r\n");

        return .{ .ok = .{ .start_off = start_off, .total_len = hdr_len + body_len } };
    }

    pub fn checkPostExists(self: *Db, id: i64) ExistsResult {
        if (!self.beginTx()) return .err;
        const cp = td_cursor(POSTS);
        const res = td_seek(cp, id);
        if (res == 1) return .exists;
        if (res == 0) return .not_found;
        return .err;
    }

    pub fn likePost(self: *Db, uid: i64, post_id: i64, out: *Buf) LikeResult {
        if (!self.beginTx()) return .err;
        const exists = self.checkPostExists(post_id);
        if (exists == .not_found) return .not_found;
        if (exists == .err) return .err;

        const rc = td_insert_like(uid, post_id);
        if (rc < 0) return .err;

        if (rc > 0) {
            out.writeAll("{\"liked\":true,\"already_liked\":false,\"post_id\":");
            out.writeU64(@intCast(post_id));
            out.writeByte('}');
            return .created;
        } else {
            out.writeAll("{\"liked\":true,\"already_liked\":true,\"post_id\":");
            out.writeU64(@intCast(post_id));
            out.writeByte('}');
            return .already_liked;
        }
    }

    pub fn unlikePost(self: *Db, uid: i64, post_id: i64, out: *Buf) UnlikeResult {
        if (!self.beginTx()) return .err;
        const exists = self.checkPostExists(post_id);
        if (exists == .not_found) return .not_found;
        if (exists == .err) return .err;

        const rc = td_delete_like(uid, post_id);
        if (rc < 0) return .err;

        out.writeAll("{\"liked\":false,\"post_id\":");
        out.writeU64(@intCast(post_id));
        out.writeByte('}');
        return .ok;
    }

    pub fn createPost(self: *Db, uid: i64, author: []const u8, body: []const u8, out: *Buf) !void {
        if (!self.beginTx()) return error.InsertFailed;
        var id: i64 = 0;
        var created_buf: [64]u8 = undefined;
        var created_len: c_int = 0;

        if (td_insert_post(uid, body.ptr, @intCast(body.len), &id, &created_buf, &created_len) != 0) {
            return error.InsertFailed;
        }

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(created_buf[0..@intCast(created_len)]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author);
        out.writeAll("\",\"like_count\":0}}");
    }

    pub fn health(self: *Db, out: *Buf, uptime: i64) !bool {
        _ = self;
        var err_buf: [256]u8 = undefined;
        if (td_health(&err_buf, 256) == 0) {
            out.writeAll("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            out.writeU64(@intCast(uptime));
            out.writeByte('}');
            return true;
        } else {
            return false;
        }
    }
};
