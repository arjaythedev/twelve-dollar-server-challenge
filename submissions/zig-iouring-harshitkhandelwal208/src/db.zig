const std = @import("std");
const sys = @import("sys.zig");
const bufm = @import("buf.zig");
const Buf = bufm.Buf;

pub const c = @import("c");

pub const Db = struct {
    handle: ?*c.sqlite3 = null,
    st_feed: ?*c.sqlite3_stmt = null,
    st_post: ?*c.sqlite3_stmt = null,
    st_check: ?*c.sqlite3_stmt = null,
    st_insert: ?*c.sqlite3_stmt = null,
    st_like: ?*c.sqlite3_stmt = null,
    st_unlike: ?*c.sqlite3_stmt = null,
    st_one: ?*c.sqlite3_stmt = null,
    st_begin: ?*c.sqlite3_stmt = null,
    st_commit: ?*c.sqlite3_stmt = null,
    st_rollback: ?*c.sqlite3_stmt = null,

    pub fn init(path: []const u8) !Db {
        // Transparent Hugepage arena for SQLite page cache (32 MB)
        const pcache_sz = 32 * 1024 * 1024;
        if (sys.mapAnon(pcache_sz)) |mem| {
            sys.madviseHuge(mem);
            _ = c.sqlite3_config(c.SQLITE_CONFIG_PAGECACHE, mem.ptr, @as(c_int, 4096), @as(c_int, @intCast(pcache_sz / 4096)));
        } else |_| {}

        _ = c.sqlite3_initialize();

        var self: Db = .{};
        var path_z: [512:0]u8 = undefined;
        if (path.len >= 512) return error.PathTooLong;
        @memcpy(path_z[0..path.len], path);
        path_z[path.len] = 0;

        if (c.sqlite3_open_v2(&path_z, &self.handle, c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_NOMUTEX, null) != c.SQLITE_OK) {
            return error.OpenFailed;
        }

        // Pragmas
        _ = c.sqlite3_exec(self.handle, "PRAGMA journal_mode=WAL;", null, null, null);
        _ = c.sqlite3_exec(self.handle, "PRAGMA synchronous=NORMAL;", null, null, null);
        _ = c.sqlite3_exec(self.handle, "PRAGMA cache_size=-32000;", null, null, null);
        _ = c.sqlite3_exec(self.handle, "PRAGMA mmap_size=268435456;", null, null, null);
        _ = c.sqlite3_exec(self.handle, "PRAGMA temp_store=MEMORY;", null, null, null);
        _ = c.sqlite3_exec(self.handle, "PRAGMA busy_timeout=5000;", null, null, null);

        // Prepared statements
        self.st_feed = try prepare(self.handle,
            \\SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            \\FROM posts p JOIN users u ON u.id = p.user_id
            \\ORDER BY p.created_at DESC, p.id DESC LIMIT 20;
        );
        self.st_post = try prepare(self.handle,
            \\SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            \\FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?;
        );
        self.st_check = try prepare(self.handle, "SELECT 1 FROM posts WHERE id = ?;");
        self.st_insert = try prepare(self.handle, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;");
        self.st_like = try prepare(self.handle, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?, ?);");
        self.st_unlike = try prepare(self.handle, "DELETE FROM likes WHERE user_id = ? AND post_id = ?;");
        self.st_one = try prepare(self.handle, "SELECT 1;");
        self.st_begin = try prepare(self.handle, "BEGIN IMMEDIATE;");
        self.st_commit = try prepare(self.handle, "COMMIT;");
        self.st_rollback = try prepare(self.handle, "ROLLBACK;");

        return self;
    }

    fn prepare(db: ?*c.sqlite3, sql: [:0]const u8) !?*c.sqlite3_stmt {
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v3(db, sql.ptr, @intCast(sql.len), c.SQLITE_PREPARE_PERSISTENT, &stmt, null) != c.SQLITE_OK) {
            return error.PrepareFailed;
        }
        return stmt;
    }

    pub fn feed(self: *Db, out: *Buf) !void {
        _ = c.sqlite3_reset(self.st_feed);
        out.writeAll("{\"posts\":[");
        var i: usize = 0;
        while (c.sqlite3_step(self.st_feed) == c.SQLITE_ROW) : (i += 1) {
            if (i > 0) out.writeByte(',');
            writePostRow(self.st_feed, out);
        }
        _ = c.sqlite3_reset(self.st_feed);
        out.writeAll("]}");
    }

    fn writePostRow(stmt: ?*c.sqlite3_stmt, out: *Buf) void {
        const id = c.sqlite3_column_int64(stmt, 0);
        const body_ptr = c.sqlite3_column_text(stmt, 1);
        const body_len: usize = @intCast(c.sqlite3_column_bytes(stmt, 1));
        const date_ptr = c.sqlite3_column_text(stmt, 2);
        const date_len: usize = @intCast(c.sqlite3_column_bytes(stmt, 2));
        const author_ptr = c.sqlite3_column_text(stmt, 3);
        const author_len: usize = @intCast(c.sqlite3_column_bytes(stmt, 3));
        const likes = c.sqlite3_column_int64(stmt, 4);

        out.writeAll("{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body_ptr[0..body_len]);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(date_ptr[0..date_len]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author_ptr[0..author_len]);
        out.writeAll("\",\"like_count\":");
        out.writeU64(@intCast(likes));
        out.writeByte('}');
    }

    pub fn getPost(self: *Db, id: i64, out: *Buf) enum { ok, not_found, err } {
        _ = c.sqlite3_reset(self.st_post);
        _ = c.sqlite3_bind_int64(self.st_post, 1, id);
        const rc = c.sqlite3_step(self.st_post);
        if (rc == c.SQLITE_ROW) {
            out.writeAll("{\"post\":");
            writePostRow(self.st_post, out);
            out.writeByte('}');
            _ = c.sqlite3_reset(self.st_post);
            return .ok;
        }
        _ = c.sqlite3_reset(self.st_post);
        if (rc == c.SQLITE_DONE) return .not_found;
        return .err;
    }

    pub fn createPost(self: *Db, uid: i64, author: []const u8, body: []const u8, out: *Buf) !void {
        _ = c.sqlite3_reset(self.st_insert);
        _ = c.sqlite3_bind_int64(self.st_insert, 1, uid);
        _ = c.sqlite3_bind_text(self.st_insert, 2, body.ptr, @intCast(body.len), c.SQLITE_STATIC);
        const rc = c.sqlite3_step(self.st_insert);
        if (rc != c.SQLITE_ROW) {
            _ = c.sqlite3_reset(self.st_insert);
            return error.InsertFailed;
        }
        const id = c.sqlite3_column_int64(self.st_insert, 0);
        const date_ptr = c.sqlite3_column_text(self.st_insert, 1);
        const date_len: usize = @intCast(c.sqlite3_column_bytes(self.st_insert, 1));

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(date_ptr[0..date_len]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author);
        out.writeAll("\",\"like_count\":0}}");

        _ = c.sqlite3_step(self.st_insert);
        _ = c.sqlite3_reset(self.st_insert);
    }

    pub fn likePost(self: *Db, uid: i64, post_id: i64, out: *Buf) enum { created, already_liked, not_found, err } {
        // 1. Verify post exists
        _ = c.sqlite3_reset(self.st_check);
        _ = c.sqlite3_bind_int64(self.st_check, 1, post_id);
        const rc_check = c.sqlite3_step(self.st_check);
        _ = c.sqlite3_reset(self.st_check);
        if (rc_check == c.SQLITE_DONE) return .not_found;
        if (rc_check != c.SQLITE_ROW) return .err;

        // 2. Insert like
        _ = c.sqlite3_reset(self.st_like);
        _ = c.sqlite3_bind_int64(self.st_like, 1, uid);
        _ = c.sqlite3_bind_int64(self.st_like, 2, post_id);
        const rc = c.sqlite3_step(self.st_like);
        _ = c.sqlite3_reset(self.st_like);
        if (rc != c.SQLITE_DONE) return .err;

        const changes = c.sqlite3_changes(self.handle);
        if (changes > 0) {
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

    pub fn unlikePost(self: *Db, uid: i64, post_id: i64, out: *Buf) enum { ok, not_found, err } {
        // 1. Verify post exists
        _ = c.sqlite3_reset(self.st_check);
        _ = c.sqlite3_bind_int64(self.st_check, 1, post_id);
        const rc_check = c.sqlite3_step(self.st_check);
        _ = c.sqlite3_reset(self.st_check);
        if (rc_check == c.SQLITE_DONE) return .not_found;
        if (rc_check != c.SQLITE_ROW) return .err;

        // 2. Delete like
        _ = c.sqlite3_reset(self.st_unlike);
        _ = c.sqlite3_bind_int64(self.st_unlike, 1, uid);
        _ = c.sqlite3_bind_int64(self.st_unlike, 2, post_id);
        const rc = c.sqlite3_step(self.st_unlike);
        _ = c.sqlite3_reset(self.st_unlike);
        if (rc != c.SQLITE_DONE) return .err;

        out.writeAll("{\"liked\":false,\"post_id\":");
        out.writeU64(@intCast(post_id));
        out.writeByte('}');
        return .ok;
    }

    pub fn health(self: *Db, out: *Buf, uptime: i64) !bool {
        _ = c.sqlite3_reset(self.st_one);
        const rc = c.sqlite3_step(self.st_one);
        _ = c.sqlite3_reset(self.st_one);
        if (rc == c.SQLITE_ROW) {
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
