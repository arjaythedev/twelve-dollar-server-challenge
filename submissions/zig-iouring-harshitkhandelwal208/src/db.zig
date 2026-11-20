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
extern "c" fn td_feed_ids(c: ?*anyopaque, ids: [*]i64, limit: c_int) c_int;
extern "c" fn td_next(c: ?*anyopaque) c_int;
extern "c" fn td_payload(c: ?*anyopaque, len: *u32) ?[*]const u8;
extern "c" fn td_seek_payload(c: ?*anyopaque, id: i64, data: *[*]const u8, len: *u32) c_int;
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

inline fn recordAt(cur: ?*anyopaque, id: i64) ![]const u8 {
    var data: [*]const u8 = undefined;
    var len: u32 = undefined;
    return switch (td_seek_payload(cur, id, &data, &len)) {
        1 => data[0..len],
        0 => DbError.NotFound,
        else => DbError.DbFailed,
    };
}

const PostFields = struct { uid: i64, body: []const u8, created: []const u8 };

inline fn fastDecodePost(p: []const u8) ?PostFields {
    if (p.len < 32) return null;
    const hsz = p[0];
    if (hsz < 5 or hsz >= p.len or p[1] != 0) return null;
    // The fixed-width path requires SQLite's one-byte TEXT serial type for
    // a 24-byte timestamp. Other records use the complete header decoder.
    if (p[hsz - 1] != 13 + 2 * 24 or p[hsz - 2] & 0x80 != 0) return null;
    const t_uid = p[2];
    const uid_len: usize = switch (t_uid) {
        1 => 1,
        2 => 2,
        3 => 3,
        4 => 4,
        6 => 8,
        8, 9 => 0,
        else => return null,
    };
    if (hsz + uid_len + 24 > p.len) return null;
    const puid = p[hsz .. hsz + uid_len];
    const uid: i64 = switch (t_uid) {
        1 => @as(i8, @bitCast(puid[0])),
        2 => std.mem.readInt(i16, puid[0..2], .big),
        3 => blk: {
            const raw = (@as(u32, puid[0]) << 24) | (@as(u32, puid[1]) << 16) | (@as(u32, puid[2]) << 8);
            break :blk @as(i32, @bitCast(raw)) >> 8;
        },
        4 => std.mem.readInt(i32, puid[0..4], .big),
        6 => std.mem.readInt(i64, puid[0..8], .big),
        8 => 0,
        9 => 1,
        else => return null,
    };
    const created = p[p.len - 24 .. p.len];
    const body = p[hsz + uid_len .. p.len - 24];
    return .{ .uid = uid, .body = body, .created = created };
}

inline fn fastDecodeUser(p: []const u8) ?[]const u8 {
    if (p.len < 5) return null;
    const hsz = p[0];
    if (hsz < 4 or hsz >= p.len) return null;
    if (p[1] != 0) return null; // INTEGER PRIMARY KEY alias is type 0 (NULL)
    const t_name = p[2];
    if (t_name < 13 or (t_name & 1) == 0 or t_name >= 128) return null;
    const name_len = (t_name - 13) >> 1;
    if (hsz + name_len > p.len) return null;
    return p[hsz .. hsz + name_len];
}

pub const Db = struct {
    pub const ExistsResult = enum { exists, not_found, err };
    pub const GetPostResult = enum { ok, not_found, err };
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
        const start = out.len;
        self.feedFromIndex(out) catch {
            out.len = start;
            try self.feedSql(out);
        };
    }

    fn feedFromIndex(self: *Db, out: *Buf) !void {
        if (!self.beginTx()) return DbError.DbFailed;
        const cp = td_cursor(POSTS);
        const cu = td_cursor(USERS);
        const cl = td_cursor(LIKES_IDX);
        var ids: [FEED_LEN]i64 = undefined;
        const n = td_feed_ids(td_cursor(FEED_IDX), &ids, ids.len);
        if (n < 0) return DbError.DbFailed;
        const posts = ids[0..@intCast(n)];
        out.writeAll("{\"posts\":[");
        if (posts.len > 0) {
            const lo = std.mem.min(i64, posts);
            const hi = std.mem.max(i64, posts);
            var counts: [RANGE_SPAN + 1]u32 = undefined;
            const grouped = blk: {
                if (lo < 1 or hi - lo > RANGE_SPAN) break :blk false;
                @memset(counts[0..@intCast(hi - lo + 1)], 0);
                break :blk td_count_likes(cl, lo, hi, &counts) == 0;
            };
            for (posts, 0..) |id, i| {
                const likes = if (grouped) counts[@intCast(id - lo)] else try countLikes(cl, id);
                if (i != 0) out.writeByte(',');
                try appendPost(cp, cu, id, likes, out);
            }
        }
        out.writeAll("]}");
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
        const likes = countLikes(td_cursor(LIKES_IDX), id) catch return .err;
        const start = out.len;
        out.writeAll("{\"post\":");
        appendPost(td_cursor(POSTS), td_cursor(USERS), id, likes, out) catch |err| {
            out.len = start;
            return if (err == DbError.NotFound) .not_found else .err;
        };
        out.writeByte('}');
        return .ok;
    }

    fn appendPost(cp: ?*anyopaque, cu: ?*anyopaque, id: i64, likes: i64, out: *Buf) !void {
        const p = try recordAt(cp, id);
        const post: PostFields = fastDecodePost(p) orelse blk: {
            const cols = try columns(4, p);
            break :blk .{ .uid = try intOf(cols[1].t, cols[1].b), .body = try text(cols[2]), .created = try text(cols[3]) };
        };
        const u = try recordAt(cu, post.uid);
        const author = fastDecodeUser(u) orelse try text((try columns(2, u))[1]);

        // Consume both cursor payloads while their pages are still pinned.
        // Reserve the maximum JSON expansion; the normal slab path is one comparison.
        try out.ensure(104 + post.created.len + 6 * (post.body.len + author.len));
        var ptr = out.ptr + out.len;
        ptr[0..6].* = "{\"id\":".*;
        ptr += 6;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(id));
        ptr[0..9].* = ",\"body\":\"".*;
        ptr += 9;
        ptr += bufm.writeJsonStringDirect(ptr, post.body);
        ptr[0..16].* = "\",\"created_at\":\"".*;
        ptr += 16;
        @memcpy(ptr[0..post.created.len], post.created);
        ptr += post.created.len;
        ptr[0..12].* = "\",\"author\":\"".*;
        ptr += 12;
        ptr += bufm.writeJsonStringDirect(ptr, author);
        ptr[0..15].* = "\",\"like_count\":".*;
        ptr += 15;
        ptr += bufm.writeU64Direct(ptr[0..24], @intCast(likes));
        ptr[0] = '}';
        out.len = @intFromPtr(ptr + 1) - @intFromPtr(out.ptr);
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
