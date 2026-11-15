//! High-speed SQLite database engine over direct B-tree cursors and prepared statements.
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

pub fn varintLong(p: []const u8, i: *usize) !u64 {
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
        const slen = serialLen(t);
        if (body + slen > rec.len) return DbError.Corrupt;
        cols[k] = .{
            .t = t,
            .b = rec[body .. body + slen],
        };
        body += slen;
    }
    return cols;
}

inline fn text(c_val: Col) ![]const u8 {
    if (c_val.t >= 13 and (c_val.t & 1 == 1)) return c_val.b; // 13 + 2N is text
    return DbError.Corrupt;
}

inline fn getPayload(cur: ?*anyopaque) ![]const u8 {
    var len: u32 = 0;
    const p = td_payload(cur, &len) orelse return DbError.Corrupt;
    return p[0..len];
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
    in_tx: bool = false,

    pub fn init(path: []const u8) !Db {
        var path_z: [512:0]u8 = undefined;
        if (path.len >= 512) return error.PathTooLong;
        @memcpy(path_z[0..path.len], path);
        path_z[path.len] = 0;

        var err_buf: [256]u8 = undefined;
        if (td_open(&path_z, &err_buf, err_buf.len) != 0) {
            std.log.err("td_open failed: {s}", .{err_buf});
            return error.OpenFailed;
        }

        return .{};
    }

    pub inline fn beginTx(self: *Db) bool {
        if (td_in_batch() != 0) {
            self.in_tx = true;
            return true;
        }
        const ok = (td_begin() == 0);
        self.in_tx = ok;
        return ok;
    }

    pub inline fn commitTx(self: *Db) bool {
        self.in_tx = false;
        return td_commit() == 0;
    }

    pub inline fn rollbackTx(self: *Db) void {
        self.in_tx = false;
        td_rollback();
    }

    pub fn deinit(self: *Db) void {
        self.in_tx = false;
        td_close();
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
            // Slow fallback
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
            const p = getPayload(ci) catch {
                return self.feedSql(out);
            };
            const cols = columns(3, p) catch {
                return self.feedSql(out);
            };
            const post_id = intOf(cols[1].t, cols[1].b) catch {
                return self.feedSql(out);
            };
            const created = text(cols[0]) catch {
                return self.feedSql(out);
            };
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

        // 2. Fetch posts body and user_id by rowid seek
        var min_id: i64 = rows[0].id;
        var max_id: i64 = rows[0].id;
        for (0..n_rows) |i| {
            const pid = rows[i].id;
            if (pid < min_id) min_id = pid;
            if (pid > max_id) max_id = pid;

            if (td_seek(cp, pid) != 1) return self.feedSql(out);
            const p = getPayload(cp) catch return self.feedSql(out);
            const cols = columns(3, p) catch return self.feedSql(out);
            rows[i].user_id = intOf(cols[1].t, cols[1].b) catch return self.feedSql(out);
            rows[i].body = text(cols[2]) catch return self.feedSql(out);
        }

        // 3. Fetch authors by rowid seek
        for (0..n_rows) |i| {
            if (td_seek(cu, rows[i].user_id) != 1) return self.feedSql(out);
            const p = getPayload(cu) catch return self.feedSql(out);
            const cols = columns(2, p) catch return self.feedSql(out);
            rows[i].author = text(cols[1]) catch return self.feedSql(out);
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

        // 5. Emit JSON directly into output buffer
        out.writeAll("{\"posts\":[");
        for (0..n_rows) |i| {
            if (i > 0) out.writeByte(',');
            out.writeAll("{\"id\":");
            out.writeU64(@intCast(rows[i].id));
            out.writeAll(",\"body\":\"");
            out.writeJsonString(rows[i].body);
            out.writeAll("\",\"created_at\":\"");
            out.writeAll(rows[i].created);
            out.writeAll("\",\"author\":\"");
            out.writeJsonString(rows[i].author);
            out.writeAll("\",\"like_count\":");
            out.writeU64(@intCast(rows[i].likes));
            out.writeByte('}');
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

    pub fn getPost(self: *Db, id: i64, out: *Buf) enum { ok, not_found, err } {
        if (!self.beginTx()) return .err;

        const cp = td_cursor(POSTS);
        const cu = td_cursor(USERS);
        const cl = td_cursor(LIKES_IDX);

        const seek_post = td_seek(cp, id);
        if (seek_post < 0) return .err;
        if (seek_post == 0) return .not_found;

        const p = getPayload(cp) catch return .err;
        const cols = columns(4, p) catch return .err;
        const user_id = intOf(cols[1].t, cols[1].b) catch return .err;
        const body = text(cols[2]) catch return .err;
        const created = text(cols[3]) catch return .err;

        const seek_user = td_seek(cu, user_id);
        if (seek_user < 0) return .err;
        if (seek_user == 0) return .not_found;
        const up = getPayload(cu) catch return .err;
        const ucols = columns(2, up) catch return .err;
        const author = text(ucols[1]) catch return .err;

        const likes = countLikes(cl, id) catch return .err;

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(created);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author);
        out.writeAll("\",\"like_count\":");
        out.writeU64(@intCast(likes));
        out.writeAll("}}");
        return .ok;
    }

    pub fn checkPostExists(self: *Db, id: i64) enum { exists, not_found, err } {
        if (!self.beginTx()) return .err;
        const cp = td_cursor(POSTS);
        const res = td_seek(cp, id);
        if (res == 1) return .exists;
        if (res == 0) return .not_found;
        return .err;
    }

    pub fn likePost(self: *Db, uid: i64, post_id: i64, out: *Buf) enum { created, already_liked, not_found, err } {
        if (!self.beginTx()) return .err;
        const exists = self.checkPostExists(post_id);
        if (exists == .not_found) return .not_found;
        if (exists == .err) return .err;

        const rc = td_insert_like(uid, post_id);
        if (rc == 1) {
            out.writeAll("{\"liked\":true,\"already_liked\":false,\"post_id\":");
            out.writeU64(@intCast(post_id));
            out.writeByte('}');
            return .created;
        } else if (rc == 0) {
            out.writeAll("{\"liked\":true,\"already_liked\":true,\"post_id\":");
            out.writeU64(@intCast(post_id));
            out.writeByte('}');
            return .already_liked;
        }
        return .err;
    }

    pub fn unlikePost(self: *Db, uid: i64, post_id: i64, out: *Buf) enum { ok, not_found, err } {
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
        var clen: c_int = 0;
        if (td_insert_post(uid, body.ptr, @intCast(body.len), &id, &created_buf, &clen) != 0) {
            return error.InsertFailed;
        }

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(created_buf[0..@intCast(clen)]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author);
        out.writeAll("\",\"like_count\":0}}");
    }

    pub fn health(self: *Db, out: *Buf, uptime: i64) !bool {
        _ = self;
        var err_buf: [256]u8 = undefined;
        if (td_health(&err_buf, err_buf.len) == 0) {
            out.writeAll("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            out.writeU64(@intCast(uptime));
            out.writeByte('}');
            return true;
        } else {
            out.writeAll("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database error\"}");
            return false;
        }
    }
};
