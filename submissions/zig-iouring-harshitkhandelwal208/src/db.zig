//! High-speed native Zig SQLite database engine over persistent prepared statements
//! and background WAL checkpoint coordinator.
const std = @import("std");
const bufm = @import("buf.zig");
const Buf = bufm.Buf;
const linux = std.os.linux;

pub const c = @import("c");

pub const DbError = error{
    Corrupt,
    NotFound,
    DbFailed,
    InsertFailed,
    OpenFailed,
    PathTooLong,
};

pub const Db = struct {
    pub const ExistsResult = enum { exists, not_found, err };
    pub const GetPostResult = enum { ok, not_found, err };
    pub const LikeResult = enum { created, already_liked, not_found, err };
    pub const UnlikeResult = enum { ok, not_found, err };

    handle: ?*c.sqlite3 = null,
    ckdb: ?*c.sqlite3 = null,
    in_tx: bool = false,
    batch_broken: bool = false,

    // Prepared statements
    st_begin: ?*c.sqlite3_stmt = null,
    st_commit: ?*c.sqlite3_stmt = null,
    st_rollback: ?*c.sqlite3_stmt = null,
    st_post: ?*c.sqlite3_stmt = null,
    st_like: ?*c.sqlite3_stmt = null,
    st_unlike: ?*c.sqlite3_stmt = null,
    st_check: ?*c.sqlite3_stmt = null,
    st_health: ?*c.sqlite3_stmt = null,
    st_feed: ?*c.sqlite3_stmt = null,
    st_post_id: ?*c.sqlite3_stmt = null,

    // Background checkpoint worker
    ckpt_thread: ?std.Thread = null,
    ckpt_running: std.atomic.Value(bool) = std.atomic.Value(bool).init(true),
    ckpt_state: std.atomic.Value(u32) = std.atomic.Value(u32).init(ST_IDLE),
    ckpt_full: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    writes_held: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    hold_t0: i64 = 0,
    wal_frames: std.atomic.Value(i32) = std.atomic.Value(i32).init(0),

    const ST_IDLE: u32 = 0;
    const ST_BULK_REQ: u32 = 1;
    const ST_BULK_DONE: u32 = 2;
    const ST_TAIL_REQ: u32 = 3;
    const ST_TAIL_DONE: u32 = 4;

    const HOLD_MAX_MS: i64 = 100; // 100ms maximum hold to prevent p99 tail spikes
    const CKPT_FRAMES: i32 = 4096;
    const CKPT_FORCE: i32 = 65536;

    fn walHook(arg: ?*anyopaque, db_ptr: ?*c.sqlite3, name: [*c]const u8, frames: c_int) callconv(.c) c_int {
        _ = db_ptr;
        _ = name;
        if (arg) |ptr| {
            const self: *Db = @ptrCast(@alignCast(ptr));
            self.wal_frames.store(frames, .release);
        }
        return c.SQLITE_OK;
    }

    fn prep(db_ptr: ?*c.sqlite3, sql: [:0]const u8) ?*c.sqlite3_stmt {
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v3(db_ptr, sql.ptr, -1, c.SQLITE_PREPARE_PERSISTENT, &stmt, null) != c.SQLITE_OK) {
            return null;
        }
        return stmt;
    }

    fn exec(db_ptr: ?*c.sqlite3, sql: [:0]const u8) c_int {
        return c.sqlite3_exec(db_ptr, sql.ptr, null, null, null);
    }

    fn ckptMain(self: *Db) void {
        const tid: linux.pid_t = @intCast(linux.gettid());
        _ = linux.syscall3(.setpriority, 0, @as(usize, @bitCast(@as(isize, tid))), 19);

        while (self.ckpt_running.load(.acquire)) {
            var st = self.ckpt_state.load(.acquire);
            while (self.ckpt_running.load(.acquire) and st != ST_BULK_REQ and st != ST_TAIL_REQ) {
                const ts = linux.timespec{ .sec = 1, .nsec = 0 };
                _ = linux.futex_4arg(&self.ckpt_state.raw, .{ .cmd = .WAIT, .private = true }, st, &ts);
                st = self.ckpt_state.load(.acquire);
            }
            if (!self.ckpt_running.load(.acquire)) break;

            var nlog: c_int = -1;
            var ndone: c_int = -2;
            const rc = c.sqlite3_wal_checkpoint_v2(self.ckdb, "main", c.SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
            self.ckpt_full.store(rc == c.SQLITE_OK and nlog == ndone, .release);

            const next_st: u32 = if (st == ST_BULK_REQ) ST_BULK_DONE else ST_TAIL_DONE;
            self.ckpt_state.store(next_st, .release);
            _ = linux.futex_3arg(&self.ckpt_state.raw, .{ .cmd = .WAKE, .private = true }, 1);
        }
    }

    fn stepReset(stmt: ?*c.sqlite3_stmt) c_int {
        const rc = c.sqlite3_step(stmt);
        _ = c.sqlite3_reset(stmt);
        return rc;
    }

    fn checkTx(self: *Db) void {
        if (self.in_tx and c.sqlite3_get_autocommit(self.handle) != 0) {
            self.in_tx = false;
            self.batch_broken = true;
        }
    }

    pub fn init(path: []const u8) !Db {
        var self: Db = .{};
        errdefer self.deinit();

        var path_z: [512:0]u8 = undefined;
        if (path.len >= 512) return error.PathTooLong;
        @memcpy(path_z[0..path.len], path);
        path_z[path.len] = 0;

        if (c.sqlite3_initialize() != c.SQLITE_OK) return error.OpenFailed;

        // Open primary connection
        if (c.sqlite3_open_v2(&path_z, &self.handle, c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_NOMUTEX, null) != c.SQLITE_OK) {
            return error.OpenFailed;
        }

        // Apply performance pragmas on primary connection
        if (exec(self.handle, "PRAGMA journal_mode = WAL") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA synchronous = NORMAL") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA temp_store = MEMORY") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA cache_size = -8192") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA mmap_size = 0") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA busy_timeout = 5000") != c.SQLITE_OK or
            exec(self.handle, "PRAGMA wal_autocheckpoint = 0") != c.SQLITE_OK)
        {
            return error.OpenFailed;
        }

        _ = c.sqlite3_wal_hook(self.handle, walHook, @ptrCast(&self));

        // Open checkpoint connection
        if (c.sqlite3_open_v2(&path_z, &self.ckdb, c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_NOMUTEX, null) != c.SQLITE_OK) {
            return error.OpenFailed;
        }
        _ = c.sqlite3_busy_timeout(self.ckdb, 5000);
        if (exec(self.ckdb, "PRAGMA synchronous = NORMAL") != c.SQLITE_OK or
            exec(self.ckdb, "PRAGMA wal_autocheckpoint = 0") != c.SQLITE_OK or
            exec(self.ckdb, "PRAGMA cache_size = 16") != c.SQLITE_OK)
        {
            return error.OpenFailed;
        }

        // Prepare persistent SQL statements
        self.st_begin = prep(self.handle, "BEGIN") orelse return error.OpenFailed;
        self.st_commit = prep(self.handle, "COMMIT") orelse return error.OpenFailed;
        self.st_rollback = prep(self.handle, "ROLLBACK") orelse return error.OpenFailed;
        self.st_post = prep(self.handle, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at") orelse return error.OpenFailed;
        self.st_like = prep(self.handle, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING") orelse return error.OpenFailed;
        self.st_unlike = prep(self.handle, "DELETE FROM likes WHERE user_id = ?1 AND post_id = ?2") orelse return error.OpenFailed;
        self.st_check = prep(self.handle, "SELECT 1 FROM posts WHERE id = ?1") orelse return error.OpenFailed;
        self.st_health = prep(self.handle, "SELECT 1") orelse return error.OpenFailed;
        self.st_feed = prep(self.handle, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20") orelse return error.OpenFailed;
        self.st_post_id = prep(self.handle, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1") orelse return error.OpenFailed;

        // Spawn low-priority checkpoint coordinator thread
        self.ckpt_running.store(true, .release);
        self.ckpt_thread = try std.Thread.spawn(.{}, ckptMain, .{&self});

        return self;
    }

    pub fn deinit(self: *Db) void {
        self.in_tx = false;
        self.batch_broken = false;
        self.ckpt_running.store(false, .release);
        self.ckpt_state.store(0xffffffff, .release);
        _ = linux.futex_3arg(&self.ckpt_state.raw, .{ .cmd = .WAKE, .private = true }, 1);

        if (self.ckpt_thread) |t| {
            t.join();
            self.ckpt_thread = null;
        }

        if (self.st_begin) |s| _ = c.sqlite3_finalize(s);
        if (self.st_commit) |s| _ = c.sqlite3_finalize(s);
        if (self.st_rollback) |s| _ = c.sqlite3_finalize(s);
        if (self.st_post) |s| _ = c.sqlite3_finalize(s);
        if (self.st_like) |s| _ = c.sqlite3_finalize(s);
        if (self.st_unlike) |s| _ = c.sqlite3_finalize(s);
        if (self.st_check) |s| _ = c.sqlite3_finalize(s);
        if (self.st_health) |s| _ = c.sqlite3_finalize(s);
        if (self.st_feed) |s| _ = c.sqlite3_finalize(s);
        if (self.st_post_id) |s| _ = c.sqlite3_finalize(s);

        if (self.ckdb) |d| _ = c.sqlite3_close(d);
        if (self.handle) |d| _ = c.sqlite3_close(d);
        self.* = .{};
    }

    pub fn beginTx(self: *Db) bool {
        if (self.batch_broken) return false;
        if (self.in_tx) {
            if (c.sqlite3_get_autocommit(self.handle) == 0) return true;
            self.in_tx = false;
            self.batch_broken = true;
            return false;
        }
        if (stepReset(self.st_begin) != c.SQLITE_DONE) return false;
        self.in_tx = true;
        return true;
    }

    pub fn commitTx(self: *Db) bool {
        if (self.batch_broken) {
            self.batch_broken = false;
            self.in_tx = false;
            if (c.sqlite3_get_autocommit(self.handle) == 0) _ = stepReset(self.st_rollback);
            return false;
        }
        if (!self.in_tx) return true;
        self.in_tx = false;
        if (stepReset(self.st_commit) != c.SQLITE_DONE) {
            _ = stepReset(self.st_rollback);
            return false;
        }
        return true;
    }

    pub fn rollbackTx(self: *Db) void {
        if (!self.in_tx) return;
        self.in_tx = false;
        _ = stepReset(self.st_rollback);
    }

    pub inline fn inBatch(self: *Db) bool {
        return self.in_tx and (c.sqlite3_get_autocommit(self.handle) == 0);
    }

    pub inline fn writesHeld(self: *Db) bool {
        return self.writes_held.load(.acquire);
    }

    fn milliTimestamp() i64 {
        var ts: linux.timespec = undefined;
        _ = linux.clock_gettime(.MONOTONIC, &ts);
        return @as(i64, @intCast(ts.sec)) * 1000 + @as(i64, @intCast(@divTrunc(ts.nsec, 1_000_000)));
    }

    pub fn tick(self: *Db) void {
        const st = self.ckpt_state.load(.acquire);
        switch (st) {
            ST_IDLE => {
                const frames = self.wal_frames.load(.acquire);
                if (frames >= CKPT_FORCE) {
                    var nlog: c_int = 0;
                    var ndone: c_int = 0;
                    if (c.sqlite3_wal_checkpoint_v2(self.handle, "main", c.SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == c.SQLITE_OK and nlog == ndone) {
                        self.wal_frames.store(0, .release);
                    }
                } else if (frames >= CKPT_FRAMES) {
                    self.ckpt_state.store(ST_BULK_REQ, .release);
                    _ = linux.futex_3arg(&self.ckpt_state.raw, .{ .cmd = .WAKE, .private = true }, 1);
                }
            },
            ST_BULK_DONE => {
                self.writes_held.store(true, .release);
                self.hold_t0 = milliTimestamp();
                self.ckpt_state.store(ST_TAIL_REQ, .release);
                _ = linux.futex_3arg(&self.ckpt_state.raw, .{ .cmd = .WAKE, .private = true }, 1);
            },
            ST_TAIL_DONE => {
                if (self.ckpt_full.load(.acquire)) self.wal_frames.store(0, .release);
                self.writes_held.store(false, .release);
                self.ckpt_state.store(ST_IDLE, .release);
            },
            else => {
                if (self.writes_held.load(.acquire)) {
                    if (milliTimestamp() - self.hold_t0 > HOLD_MAX_MS) {
                        self.writes_held.store(false, .release);
                        self.ckpt_state.store(ST_IDLE, .release);
                    }
                }
            },
        }
    }

    pub fn feed(self: *Db, out: *Buf) !void {
        if (!self.beginTx()) return DbError.DbFailed;
        _ = c.sqlite3_reset(self.st_feed);
        out.writeAll("{\"posts\":[");
        var first = true;
        while (c.sqlite3_step(self.st_feed) == c.SQLITE_ROW) {
            const id = c.sqlite3_column_int64(self.st_feed, 0);
            const body_ptr = c.sqlite3_column_text(self.st_feed, 1);
            const body_len = c.sqlite3_column_bytes(self.st_feed, 1);
            const ca_ptr = c.sqlite3_column_text(self.st_feed, 2);
            const ca_len = c.sqlite3_column_bytes(self.st_feed, 2);
            const author_ptr = c.sqlite3_column_text(self.st_feed, 3);
            const author_len = c.sqlite3_column_bytes(self.st_feed, 3);
            const likes = c.sqlite3_column_int64(self.st_feed, 4);

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
        _ = c.sqlite3_reset(self.st_post_id);
        if (c.sqlite3_bind_int64(self.st_post_id, 1, id) != c.SQLITE_OK) return .err;

        const rc = c.sqlite3_step(self.st_post_id);
        if (rc == c.SQLITE_DONE) return .not_found;
        if (rc != c.SQLITE_ROW) return .err;

        const body_ptr = c.sqlite3_column_text(self.st_post_id, 1);
        const body_len = c.sqlite3_column_bytes(self.st_post_id, 1);
        const ca_ptr = c.sqlite3_column_text(self.st_post_id, 2);
        const ca_len = c.sqlite3_column_bytes(self.st_post_id, 2);
        const author_ptr = c.sqlite3_column_text(self.st_post_id, 3);
        const author_len = c.sqlite3_column_bytes(self.st_post_id, 3);
        const likes = c.sqlite3_column_int64(self.st_post_id, 4);

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body_ptr[0..@intCast(body_len)]);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(ca_ptr[0..@intCast(ca_len)]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author_ptr[0..@intCast(author_len)]);
        out.writeAll("\",\"like_count\":");
        out.writeU64(@intCast(likes));
        out.writeAll("}}");
        return .ok;
    }

    pub fn checkPostExists(self: *Db, id: i64) ExistsResult {
        if (!self.beginTx()) return .err;
        _ = c.sqlite3_reset(self.st_check);
        if (c.sqlite3_bind_int64(self.st_check, 1, id) != c.SQLITE_OK) return .err;
        const rc = c.sqlite3_step(self.st_check);
        if (rc == c.SQLITE_ROW) return .exists;
        if (rc == c.SQLITE_DONE) return .not_found;
        return .err;
    }

    pub fn likePost(self: *Db, uid: i64, post_id: i64, out: *Buf) LikeResult {
        if (!self.beginTx()) return .err;
        const exists = self.checkPostExists(post_id);
        if (exists == .not_found) return .not_found;
        if (exists == .err) return .err;

        _ = c.sqlite3_reset(self.st_like);
        _ = c.sqlite3_bind_int64(self.st_like, 1, uid);
        _ = c.sqlite3_bind_int64(self.st_like, 2, post_id);
        if (c.sqlite3_step(self.st_like) != c.SQLITE_DONE) {
            self.checkTx();
            return .err;
        }

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

    pub fn unlikePost(self: *Db, uid: i64, post_id: i64, out: *Buf) UnlikeResult {
        if (!self.beginTx()) return .err;
        const exists = self.checkPostExists(post_id);
        if (exists == .not_found) return .not_found;
        if (exists == .err) return .err;

        _ = c.sqlite3_reset(self.st_unlike);
        _ = c.sqlite3_bind_int64(self.st_unlike, 1, uid);
        _ = c.sqlite3_bind_int64(self.st_unlike, 2, post_id);
        if (c.sqlite3_step(self.st_unlike) != c.SQLITE_DONE) {
            self.checkTx();
            return .err;
        }

        out.writeAll("{\"liked\":false,\"post_id\":");
        out.writeU64(@intCast(post_id));
        out.writeByte('}');
        return .ok;
    }

    pub fn createPost(self: *Db, uid: i64, author: []const u8, body: []const u8, out: *Buf) !void {
        if (!self.beginTx()) return error.InsertFailed;
        _ = c.sqlite3_reset(self.st_post);
        _ = c.sqlite3_bind_int64(self.st_post, 1, uid);
        _ = c.sqlite3_bind_text(self.st_post, 2, body.ptr, @intCast(body.len), c.SQLITE_STATIC);

        const rc = c.sqlite3_step(self.st_post);
        if (rc != c.SQLITE_ROW) {
            self.checkTx();
            return error.InsertFailed;
        }

        const id = c.sqlite3_column_int64(self.st_post, 0);
        const t = c.sqlite3_column_text(self.st_post, 1);
        const n = c.sqlite3_column_bytes(self.st_post, 1);
        if (t == null or n > 63) return error.InsertFailed;

        out.writeAll("{\"post\":{\"id\":");
        out.writeU64(@intCast(id));
        out.writeAll(",\"body\":\"");
        out.writeJsonString(body);
        out.writeAll("\",\"created_at\":\"");
        out.writeAll(t[0..@intCast(n)]);
        out.writeAll("\",\"author\":\"");
        out.writeJsonString(author);
        out.writeAll("\",\"like_count\":0}}");

        _ = c.sqlite3_step(self.st_post);
        _ = c.sqlite3_reset(self.st_post);
        _ = c.sqlite3_clear_bindings(self.st_post);
    }

    pub fn health(self: *Db, out: *Buf, uptime: i64) !bool {
        _ = c.sqlite3_reset(self.st_health);
        const rc = c.sqlite3_step(self.st_health);
        const ok = (rc == c.SQLITE_ROW and c.sqlite3_column_int(self.st_health, 0) == 1);
        if (ok) {
            out.writeAll("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            out.writeU64(@intCast(uptime));
            out.writeByte('}');
            return true;
        } else {
            return false;
        }
    }
};
