//! The API: routing, SQLite, auth and response bodies. `handle` never
//! suspends, so the connection task runs it on the scheduler's stack.

const App = @This();

io: Io,
db: *sqlite.Db,
st: Statements,
/// HMAC-SHA256 state already keyed with JWT_SECRET; copied per token.
keyed: jwt.HmacSha256,
start_s: i64,

/// The open write transaction, committed by `committer` once the tasks that
/// were ready alongside its first write have run.
batch: ?*Batch = null,
batch_opened: Io.Event = .unset,
/// WAL frames after the last commit, read by the checkpoint thread.
wal_frames: std.atomic.Value(u32) = .init(0),
/// Frames the checkpoint thread has copied into the database.
backfilled: std.atomic.Value(u32) = .init(0),
/// Set while the committer waits for the checkpoint thread to catch up: the
/// thread then polls fast.
draining: std.atomic.Value(bool) = .init(false),
/// Writes wait for this while the WAL drains.
writes_open: Io.Event = .is_set,
/// Drain the WAL once a commit leaves it this many frames long.
restart_at: u32 = restart_frames,

pub const Batch = struct {
    done: Io.Event = .unset,
    failed: bool = false,
    waiters: u32 = 0,
};

const Statements = struct {
    ping: *sqlite.Stmt,
    feed_ids: *sqlite.Stmt,
    feed_likes: *sqlite.Stmt,
    feed_rows: *sqlite.Stmt,
    feed_full: *sqlite.Stmt,
    post: *sqlite.Stmt,
    exists: *sqlite.Stmt,
    create: *sqlite.Stmt,
    like: *sqlite.Stmt,
    begin: *sqlite.Stmt,
    commit: *sqlite.Stmt,
    rollback: *sqlite.Stmt,
};

const post_select =
    "SELECT p.id, p.body, p.created_at, u.username, " ++
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) " ++
    "FROM posts p JOIN users u ON u.id = p.user_id ";

/// The checkpoint thread copies the WAL into the database once it has this
/// many frames. A checkpoint fsyncs twice, which took 79 ms on NVMe: on the
/// event loop that would stall every connection.
const checkpoint_frames = 1000;
const checkpoint_poll_ms = 100;
/// A checkpoint copies frames but never shrinks the WAL: the writer restarts
/// it from the beginning only if, when its transaction begins, every frame is
/// already copied. Under constant writes that moment never comes on its own,
/// so past this size the committer holds writes until the checkpoint thread
/// catches up. 8192 frames of 4 KiB pages is 32 MiB.
const restart_frames = 8192;
/// Give up on a drain after this long and try again once the WAL has grown
/// by another `restart_frames`.
const drain_timeout_ms = 500;

/// SQLite's "unix-excl" VFS: the process holds one exclusive lock on the
/// database and keeps the WAL index in its own memory, so our two connections
/// coordinate in-process. The default "unix" VFS takes and releases fcntl
/// locks around every transaction: 9% of the server's CPU at 64k users.
const vfs = "unix-excl";

pub fn init(app: *App, io: Io, path: [:0]const u8, secret: []const u8) !void {
    // Built with SQLITE_OMIT_AUTOINIT.
    if (sqlite.sqlite3_initialize() != sqlite.OK) return error.Sqlite;
    var db: ?*sqlite.Db = null;
    if (sqlite.sqlite3_open_v2(path, &db, sqlite.OPEN_READWRITE | sqlite.OPEN_NOMUTEX, vfs) != sqlite.OK) {
        std.log.err("open {s}: {s}", .{ path, if (db) |d| sqlite.sqlite3_errmsg(d) else "out of memory" });
        return error.OpenDatabase;
    }
    try exec(db.?, "PRAGMA journal_mode=WAL");
    try exec(db.?, "PRAGMA synchronous=NORMAL");
    try exec(db.?, "PRAGMA temp_store=MEMORY");
    // The database stays in the OS page cache; a bigger SQLite cache measured
    // the same CPU and p99 at 64k users for 26 MB more RSS.
    try exec(db.?, "PRAGMA cache_size=-2048");
    try exec(db.?, "PRAGMA journal_size_limit=33554432");
    try exec(db.?, "PRAGMA wal_autocheckpoint=0");
    app.* = .{
        .io = io,
        .db = db.?,
        .st = undefined,
        .keyed = .init(secret),
        .start_s = nowSeconds(io),
    };
    inline for (@typeInfo(Statements).@"struct".field_names) |name| {
        @field(app.st, name) = try prepare(db.?, @field(sql, name));
    }
    _ = sqlite.sqlite3_wal_hook(db.?, walHook, app);

    var ckpt_db: ?*sqlite.Db = null;
    if (sqlite.sqlite3_open_v2(path, &ckpt_db, sqlite.OPEN_READWRITE | sqlite.OPEN_NOMUTEX, vfs) != sqlite.OK) {
        std.log.err("open {s} for checkpoints: {s}", .{ path, if (ckpt_db) |d| sqlite.sqlite3_errmsg(d) else "out of memory" });
        return error.OpenDatabase;
    }
    try exec(ckpt_db.?, "PRAGMA busy_timeout=5000");
    // Reading the database opens its WAL; until then a checkpoint on this
    // connection silently does nothing (log and checkpointed frames are -1).
    try exec(ckpt_db.?, "PRAGMA journal_mode=WAL");
    checkpoint_db = ckpt_db.?;
    var thread: std.c.pthread_t = undefined;
    const rc = std.c.pthread_create(&thread, null, checkpointerEntry, app);
    if (rc != .SUCCESS) {
        std.log.err("checkpoint thread: {t}", .{rc});
        return error.Thread;
    }
}

var checkpoint_db: *sqlite.Db = undefined;

fn checkpointerEntry(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    checkpointer(@ptrCast(@alignCast(arg)), checkpoint_db);
}

/// Runs on its own thread with its own connection, so the fsyncs of a
/// checkpoint never block the event loop. PASSIVE never waits for readers or
/// the writer; the writer restarts the WAL once a checkpoint has caught up
/// (see `drainWal`).
fn checkpointer(app: *App, db: *sqlite.Db) noreturn {
    const linux = std.os.linux;
    // On one vCPU, only run when the event loop is idle: copying pages must
    // not preempt requests. (A setting of our own thread, not system tuning.)
    const param: linux.sched_param = .{ .priority = 0 };
    if (linux.errno(linux.sched_setscheduler(0, .{ .mode = .IDLE }, &param)) != .SUCCESS)
        std.log.warn("checkpoint thread: SCHED_IDLE unavailable", .{});
    // WAL length at the last checkpoint. Shorter now means it restarted.
    var last_log: u32 = 0;
    while (true) {
        const draining = app.draining.load(.acquire);
        const poll_ms: u32 = if (draining) 1 else checkpoint_poll_ms;
        const ts: linux.timespec = .{ .sec = 0, .nsec = poll_ms * std.time.ns_per_ms };
        _ = linux.nanosleep(&ts, null);
        const frames = app.wal_frames.load(.monotonic);
        if (frames < last_log) last_log = 0;
        if (draining) {
            if (app.backfilled.load(.monotonic) == frames) continue;
        } else if (frames < last_log + checkpoint_frames) continue;
        var log_frames: c_int = 0;
        var done_frames: c_int = 0;
        const rc = sqlite.sqlite3_wal_checkpoint_v2(db, null, sqlite.CHECKPOINT_PASSIVE, &log_frames, &done_frames);
        if (rc != sqlite.OK or log_frames < 0) {
            std.log.err("checkpoint: {s} (log {d} frames)", .{ sqlite.sqlite3_errmsg(db), log_frames });
            continue;
        }
        last_log = @intCast(log_frames);
        app.backfilled.store(@intCast(done_frames), .release);
    }
}

const sql = .{
    .ping = "SELECT 1",
    .feed_ids = "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20",
    .feed_likes = "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2",
    .feed_rows = "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2",
    .feed_full = post_select ++ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    .post = post_select ++ "WHERE p.id = ?1",
    .exists = "SELECT 1 FROM posts WHERE id = ?1",
    .create = "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
    .like = "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) " ++
        "ON CONFLICT (user_id, post_id) DO NOTHING",
    .begin = "BEGIN IMMEDIATE",
    .commit = "COMMIT",
    .rollback = "ROLLBACK",
};

fn exec(db: *sqlite.Db, text: [:0]const u8) !void {
    if (sqlite.sqlite3_exec(db, text, null, null, null) != sqlite.OK) {
        std.log.err("{s}: {s}", .{ text, sqlite.sqlite3_errmsg(db) });
        return error.Sqlite;
    }
}

fn prepare(db: *sqlite.Db, text: []const u8) !*sqlite.Stmt {
    var stmt: ?*sqlite.Stmt = null;
    if (sqlite.sqlite3_prepare_v3(db, text.ptr, @intCast(text.len), sqlite.PREPARE_PERSISTENT, &stmt, null) != sqlite.OK) {
        std.log.err("prepare {s}: {s}", .{ text, sqlite.sqlite3_errmsg(db) });
        return error.Sqlite;
    }
    return stmt.?;
}

fn walHook(arg: ?*anyopaque, db: *sqlite.Db, name: [*:0]const u8, frames: c_int) callconv(.c) c_int {
    _ = db;
    _ = name;
    const app: *App = @ptrCast(@alignCast(arg));
    app.wal_frames.store(@intCast(frames), .monotonic);
    return sqlite.OK;
}

fn nowSeconds(io: Io) i64 {
    return @intCast(@divFloor(Io.Timestamp.now(io, .real).nanoseconds, std.time.ns_per_s));
}

// ---------------------------------------------------------------------------
// Group commit

/// Commits each batch of writes. Runs as its own task.
pub fn committer(app: *App) void {
    while (true) {
        app.batch_opened.waitUncancelable(app.io);
        app.batch_opened.reset();
        const batch = app.batch orelse continue;
        app.batch = null;
        batch.failed = !UringIo.callOnSchedulerStack(app.io, commit, .{app});
        batch.done.set(app.io);
        if (batch.waiters == 0) std.heap.c_allocator.destroy(batch);
        if (app.wal_frames.load(.monotonic) >= app.restart_at) app.drainWal();
    }
}

/// Holds new writes until the checkpoint thread has copied the whole WAL, so
/// the next write transaction restarts it. Reads go on; a drain costs writes
/// one checkpoint of the frames since the last one (two fsyncs).
fn drainWal(app: *App) void {
    const frames = app.wal_frames.load(.monotonic);
    app.writes_open.reset();
    defer app.writes_open.set(app.io);
    app.backfilled.store(0, .monotonic);
    app.draining.store(true, .release);
    defer app.draining.store(false, .release);
    const start = Io.Clock.awake.now(app.io);
    while (app.backfilled.load(.acquire) != frames) {
        if (start.untilNow(app.io, .awake).toMilliseconds() >= drain_timeout_ms) {
            std.log.warn("WAL drain timed out at {d} of {d} frames", .{ app.backfilled.load(.monotonic), frames });
            app.restart_at = frames + restart_frames;
            return;
        }
        app.io.sleep(.fromMilliseconds(1), .awake) catch {};
    }
    app.restart_at = restart_frames;
}

/// Waits while the WAL drains. Call before `handle` for requests that write.
pub fn waitWritable(app: *App) void {
    app.writes_open.waitUncancelable(app.io);
}

fn commit(app: *App) bool {
    const ok = step(app.st.commit) == sqlite.DONE;
    _ = sqlite.sqlite3_reset(app.st.commit);
    if (!ok) {
        std.log.err("COMMIT: {s}", .{sqlite.sqlite3_errmsg(app.db)});
        _ = step(app.st.rollback);
        _ = sqlite.sqlite3_reset(app.st.rollback);
        return false;
    }
    return true;
}

/// Opens (or joins) the batch's write transaction. Null if BEGIN failed.
fn joinBatch(app: *App) ?*Batch {
    const batch = app.batch orelse blk: {
        const ok = step(app.st.begin) == sqlite.DONE;
        _ = sqlite.sqlite3_reset(app.st.begin);
        if (!ok) {
            std.log.err("BEGIN: {s}", .{sqlite.sqlite3_errmsg(app.db)});
            return null;
        }
        const b = std.heap.c_allocator.create(Batch) catch {
            _ = step(app.st.rollback);
            _ = sqlite.sqlite3_reset(app.st.rollback);
            return null;
        };
        b.* = .{};
        app.batch = b;
        app.batch_opened.set(app.io);
        break :blk b;
    };
    batch.waiters += 1;
    return batch;
}

/// Waits until `batch` is committed; false if the commit failed.
pub fn awaitCommit(app: *App, batch: *Batch) bool {
    batch.done.waitUncancelable(app.io);
    const ok = !batch.failed;
    batch.waiters -= 1;
    if (batch.waiters == 0) std.heap.c_allocator.destroy(batch);
    return ok;
}

// ---------------------------------------------------------------------------
// Requests

pub const Response = struct {
    /// Head and body, in `scratch`: valid until the next `handle`.
    bytes: []const u8,
    /// Hold the response until this batch commits (send a 500 if it fails).
    batch: ?*Batch = null,
};

/// Shared by every request; `handle` never suspends.
var scratch: [512 * 1024]u8 = undefined;
const head_room = 160;

const Status = enum(u16) {
    ok = 200,
    created = 201,
    bad_request = 400,
    unauthorized = 401,
    not_found = 404,
    internal_server_error = 500,
    service_unavailable = 503,

    fn line(s: Status) []const u8 {
        return switch (s) {
            .ok => "HTTP/1.1 200 OK\r\n",
            .created => "HTTP/1.1 201 Created\r\n",
            .bad_request => "HTTP/1.1 400 Bad Request\r\n",
            .unauthorized => "HTTP/1.1 401 Unauthorized\r\n",
            .not_found => "HTTP/1.1 404 Not Found\r\n",
            .internal_server_error => "HTTP/1.1 500 Internal Server Error\r\n",
            .service_unavailable => "HTTP/1.1 503 Service Unavailable\r\n",
        };
    }
};

const Body = struct {
    w: Io.Writer = .fixed(scratch[head_room..]),
};

/// Puts the head in front of the body in `scratch`.
fn finish(status: Status, w: *Io.Writer, keep_alive: bool) []const u8 {
    const body = w.buffered();
    var head_buf: [head_room]u8 = undefined;
    const head = std.fmt.bufPrint(&head_buf, "{s}Content-Type: application/json\r\nContent-Length: {d}\r\n{s}\r\n", .{
        status.line(),
        body.len,
        if (keep_alive) "" else "Connection: close\r\n",
    }) catch unreachable;
    const start = head_room - head.len;
    @memcpy(scratch[start..head_room], head);
    return scratch[start .. head_room + body.len];
}

fn errorResponse(status: Status, message: []const u8, keep_alive: bool) []const u8 {
    var b: Body = .{};
    b.w.print("{{\"error\":\"{s}\"}}", .{message}) catch unreachable;
    return finish(status, &b.w, keep_alive);
}

/// A canned 500 for when a commit fails after the response was built.
pub fn internalError(keep_alive: bool) []const u8 {
    return errorResponse(.internal_server_error, "internal server error", keep_alive);
}

pub fn handle(app: *App, req: *const http.Request) Response {
    const ka = req.keep_alive;
    return app.route(req) catch |err| .{ .bytes = switch (err) {
        error.NotFound => errorResponse(.not_found, "not found", ka),
        error.InvalidPostId => errorResponse(.bad_request, "invalid post id", ka),
        error.PostNotFound => errorResponse(.not_found, "post not found", ka),
        error.MissingBearer => errorResponse(.unauthorized, "missing bearer token", ka),
        error.InvalidToken => errorResponse(.unauthorized, "invalid or expired token", ka),
        error.InvalidTokenPayload => errorResponse(.unauthorized, "invalid token payload", ka),
        error.MalformedJson => errorResponse(.bad_request, "malformed JSON body", ka),
        error.BodyRequired => errorResponse(.bad_request, "body is required", ka),
        error.BodyTooLong => errorResponse(.bad_request, "body must be at most 500 characters", ka),
        error.Internal, error.WriteFailed => errorResponse(.internal_server_error, "internal server error", ka),
    } };
}

const RouteError = error{
    NotFound,
    InvalidPostId,
    PostNotFound,
    MissingBearer,
    InvalidToken,
    InvalidTokenPayload,
    MalformedJson,
    BodyRequired,
    BodyTooLong,
    Internal,
    WriteFailed,
};

fn route(app: *App, req: *const http.Request) RouteError!Response {
    const path = req.path;
    switch (req.method) {
        .get => {
            if (std.mem.eql(u8, path, "/feed")) return app.feed(req);
            if (std.mem.startsWith(u8, path, "/posts/")) {
                const id = jwt.parsePositive(path["/posts/".len..]) orelse return error.InvalidPostId;
                return app.getPost(req, id);
            }
            if (std.mem.eql(u8, path, "/health")) return app.health(req);
        },
        .post => {
            if (std.mem.eql(u8, path, "/posts")) return app.createPost(req);
            if (std.mem.startsWith(u8, path, "/posts/") and std.mem.endsWith(u8, path, "/like")) {
                const id_text = path["/posts/".len .. path.len - "/like".len];
                return app.like(req, id_text);
            }
        },
        .other => {},
    }
    return error.NotFound;
}

fn health(app: *App, req: *const http.Request) RouteError!Response {
    var b: Body = .{};
    const ok = step(app.st.ping) == sqlite.ROW;
    _ = sqlite.sqlite3_reset(app.st.ping);
    if (!ok) {
        try b.w.writeAll("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        try json.writeEscaped(&b.w, std.mem.span(sqlite.sqlite3_errmsg(app.db)));
        try b.w.writeAll("\"}");
        return .{ .bytes = finish(.service_unavailable, &b.w, req.keep_alive) };
    }
    try b.w.print("{{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":{d}}}", .{nowSeconds(app.io) - app.start_s});
    return .{ .bytes = finish(.ok, &b.w, req.keep_alive) };
}

fn writePost(w: *Io.Writer, id: i64, body: []const u8, created_at: []const u8, author: []const u8, likes: i64) !void {
    try w.print("{{\"id\":{d},\"body\":\"", .{id});
    try json.writeEscaped(w, body);
    try w.writeAll("\",\"created_at\":\"");
    try json.writeEscaped(w, created_at);
    try w.writeAll("\",\"author\":\"");
    try json.writeEscaped(w, author);
    try w.print("\",\"like_count\":{d}}}", .{likes});
}

fn writePostRow(w: *Io.Writer, stmt: *sqlite.Stmt) !void {
    try writePost(
        w,
        sqlite.sqlite3_column_int64(stmt, 0),
        sqlite.columnText(stmt, 1),
        sqlite.columnText(stmt, 2),
        sqlite.columnText(stmt, 3),
        sqlite.sqlite3_column_int64(stmt, 4),
    );
}

fn getPost(app: *App, req: *const http.Request, id: i64) RouteError!Response {
    const s = app.st.post;
    defer _ = sqlite.sqlite3_reset(s);
    _ = sqlite.sqlite3_bind_int64(s, 1, id);
    switch (step(s)) {
        sqlite.ROW => {},
        sqlite.DONE => return error.PostNotFound,
        else => return app.sqlError("post"),
    }
    var b: Body = .{};
    try b.w.writeAll("{\"post\":");
    try writePostRow(&b.w, s);
    try b.w.writeAll("}");
    return .{ .bytes = finish(.ok, &b.w, req.keep_alive) };
}

/// The 20 newest posts are almost always a narrow id range, so three range
/// scans (ids, like counts, rows) beat 20 correlated count(*) lookups.
const feed_span = 256;

fn feed(app: *App, req: *const http.Request) RouteError!Response {
    var ids: [20]i64 = undefined;
    var n: usize = 0;
    {
        const s = app.st.feed_ids;
        defer _ = sqlite.sqlite3_reset(s);
        while (true) switch (step(s)) {
            sqlite.ROW => {
                ids[n] = sqlite.sqlite3_column_int64(s, 0);
                n += 1;
            },
            sqlite.DONE => break,
            else => return app.sqlError("feed ids"),
        };
    }
    var b: Body = .{};
    try b.w.writeAll("{\"posts\":[");
    const by_range = n > 0 and std.mem.max(i64, ids[0..n]) - std.mem.min(i64, ids[0..n]) < feed_span and
        try app.feedByRange(&b.w, ids[0..n]);
    if (!by_range) {
        b = .{};
        try b.w.writeAll("{\"posts\":[");
        const s = app.st.feed_full;
        defer _ = sqlite.sqlite3_reset(s);
        var first = true;
        while (true) switch (step(s)) {
            sqlite.ROW => {
                if (!first) try b.w.writeAll(",");
                first = false;
                try writePostRow(&b.w, s);
            },
            sqlite.DONE => break,
            else => return app.sqlError("feed"),
        };
    }
    try b.w.writeAll("]}");
    return .{ .bytes = finish(.ok, &b.w, req.keep_alive) };
}

/// Writes the posts `ids` (in feed order, spanning less than `feed_span`).
/// False if their text doesn't fit the buffer, which 20 long posts with
/// multi-byte characters can exceed: then the caller uses the general query.
fn feedByRange(app: *App, w: *Io.Writer, ids: []const i64) RouteError!bool {
    const min = std.mem.min(i64, ids);
    const max = std.mem.max(i64, ids);
    var likes: [feed_span]u32 = @splat(0);
    {
        const s = app.st.feed_likes;
        defer _ = sqlite.sqlite3_reset(s);
        _ = sqlite.sqlite3_bind_int64(s, 1, min);
        _ = sqlite.sqlite3_bind_int64(s, 2, max);
        while (true) switch (step(s)) {
            sqlite.ROW => likes[@intCast(sqlite.sqlite3_column_int64(s, 0) - min)] += 1,
            sqlite.DONE => break,
            else => return app.sqlError("feed likes"),
        };
    }
    // Rows come in id order; keep the feed's ones to emit in feed order.
    var rows: [feed_span]?Row = @splat(null);
    var text: [32 * 1024]u8 = undefined;
    var used: usize = 0;
    {
        const s = app.st.feed_rows;
        defer _ = sqlite.sqlite3_reset(s);
        _ = sqlite.sqlite3_bind_int64(s, 1, min);
        _ = sqlite.sqlite3_bind_int64(s, 2, max);
        while (true) switch (step(s)) {
            sqlite.ROW => {
                const id = sqlite.sqlite3_column_int64(s, 0);
                if (std.mem.indexOfScalar(i64, ids, id) == null) continue;
                var row: Row = undefined;
                inline for (.{ "body", "created_at", "author" }, 1..) |field, col| {
                    const column = sqlite.columnText(s, col);
                    if (text.len - used < column.len) return false;
                    @memcpy(text[used..][0..column.len], column);
                    @field(row, field) = text[used..][0..column.len];
                    used += column.len;
                }
                rows[@intCast(id - min)] = row;
            },
            sqlite.DONE => break,
            else => return app.sqlError("feed rows"),
        };
    }
    for (ids, 0..) |id, i| {
        const slot: usize = @intCast(id - min);
        // Posts are never deleted, and nothing writes between these queries.
        const row = rows[slot] orelse {
            std.log.err("feed: post {d} missing from its id range", .{id});
            return error.Internal;
        };
        if (i != 0) try w.writeAll(",");
        try writePost(w, id, row.body, row.created_at, row.author, likes[slot]);
    }
    return true;
}

const Row = struct { body: []const u8, created_at: []const u8, author: []const u8 };

fn authenticate(app: *App, req: *const http.Request, strings: *[jwt.max_payload]u8) RouteError!jwt.Claims {
    const auth = req.authorization orelse return error.MissingBearer;
    if (!std.mem.startsWith(u8, auth, "Bearer ")) return error.MissingBearer;
    return jwt.verify(&app.keyed, auth["Bearer ".len..], nowSeconds(app.io), strings) catch |err| switch (err) {
        error.Invalid => error.InvalidToken,
        error.BadPayload => error.InvalidTokenPayload,
    };
}

fn createPost(app: *App, req: *const http.Request) RouteError!Response {
    var token_strings: [jwt.max_payload]u8 = undefined;
    const claims = try app.authenticate(req, &token_strings);
    var strings: [http.max_request]u8 = undefined;
    const input = json.parse(struct { body: ?[]const u8 }, req.body, &strings) catch return error.MalformedJson;
    const body = json.trim(input.body orelse return error.BodyRequired);
    if (body.len == 0) return error.BodyRequired;
    if (json.codePointCount(body) > 500) return error.BodyTooLong;

    const batch = app.joinBatch() orelse return error.Internal;
    errdefer leaveBatch(batch);
    const s = app.st.create;
    defer _ = sqlite.sqlite3_reset(s);
    _ = sqlite.sqlite3_bind_int64(s, 1, claims.user_id);
    _ = sqlite.sqlite3_bind_text(s, 2, body.ptr, @intCast(body.len), sqlite.STATIC);
    if (step(s) != sqlite.ROW) return app.sqlError("create");
    var b: Body = .{};
    try b.w.writeAll("{\"post\":");
    try writePost(&b.w, sqlite.sqlite3_column_int64(s, 0), body, sqlite.columnText(s, 1), claims.username, 0);
    try b.w.writeAll("}");
    if (step(s) != sqlite.DONE) return app.sqlError("create");
    return .{ .bytes = finish(.created, &b.w, req.keep_alive), .batch = batch };
}

fn like(app: *App, req: *const http.Request, id_text: []const u8) RouteError!Response {
    var token_strings: [jwt.max_payload]u8 = undefined;
    const claims = try app.authenticate(req, &token_strings);
    const post_id = jwt.parsePositive(id_text) orelse return error.InvalidPostId;

    const batch = app.joinBatch() orelse return error.Internal;
    errdefer leaveBatch(batch);
    const s = app.st.like;
    defer _ = sqlite.sqlite3_reset(s);
    _ = sqlite.sqlite3_bind_int64(s, 1, claims.user_id);
    _ = sqlite.sqlite3_bind_int64(s, 2, post_id);
    if (step(s) != sqlite.DONE) return app.sqlError("like");
    const inserted = sqlite.sqlite3_changes(app.db) == 1;
    if (!inserted) {
        const e = app.st.exists;
        defer _ = sqlite.sqlite3_reset(e);
        _ = sqlite.sqlite3_bind_int64(e, 1, post_id);
        switch (step(e)) {
            sqlite.ROW => {},
            sqlite.DONE => return error.PostNotFound,
            else => return app.sqlError("exists"),
        }
    }
    var b: Body = .{};
    try b.w.print("{{\"liked\":true,\"already_liked\":{},\"post_id\":{d}}}", .{ !inserted, post_id });
    return .{ .bytes = finish(if (inserted) .created else .ok, &b.w, req.keep_alive), .batch = batch };
}

/// Leaves a batch this request joined but won't wait for; the committer
/// frees a batch nobody waits for.
fn leaveBatch(batch: *Batch) void {
    batch.waiters -= 1;
}

fn sqlError(app: *App, what: []const u8) error{Internal} {
    std.log.err("{s}: {s}", .{ what, sqlite.sqlite3_errmsg(app.db) });
    return error.Internal;
}

fn step(s: *sqlite.Stmt) c_int {
    return sqlite.sqlite3_step(s);
}

const std = @import("std");
const Io = std.Io;
const http = @import("http.zig");
const json = @import("json.zig");
const jwt = @import("jwt.zig");
const sqlite = @import("sqlite.zig");
const UringIo = @import("UringIo.zig");
