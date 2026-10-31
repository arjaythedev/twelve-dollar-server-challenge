const std = @import("std");
const c = @import("c.zig").lib;
pub const alloc = std.heap.c_allocator;
pub const Response = struct { status: u16, body: []const u8 };
pub const internal = Response{ .status = 500, .body = "{\"error\":\"internal server error\"}" };
const Query = enum { health, feed, post, create, like, exists };
var db: ?*c.sqlite3 = null;
var statements: [6]?*c.sqlite3_stmt = @splat(null);
var secret: []const u8 = undefined;
var started: i64 = 0;
var output: std.ArrayList(u8) = .empty;
pub var transaction = false;
var failed = false;

pub fn env(name: [*:0]const u8, fallback: [:0]const u8) [:0]const u8 {
    const value = c.getenv(name);
    return if (value == null) fallback else std.mem.span(value);
}

pub fn init() !void {
    secret = env("JWT_SECRET", "twelve-dollar-challenge");
    started = monotonic();
    if (c.sqlite3_open_v2(env("SQLITE_PATH", "seed/feed.db").ptr, &db, c.SQLITE_OPEN_READWRITE, null) != c.SQLITE_OK)
        return error.DatabaseOpen;
    _ = c.sqlite3_busy_timeout(db, 5000);
    try exec("PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA cache_size=-8192; PRAGMA mmap_size=268435456;");
    const columns = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ";
    const sql = [_][:0]const u8{
        "SELECT 1",
        columns ++ "ORDER BY p.created_at DESC,p.id DESC LIMIT 20",
        columns ++ "WHERE p.id=?1",
        "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at",
        "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT DO NOTHING",
        "SELECT 1 FROM posts WHERE id=?1",
    };
    for (sql, &statements) |text, *prepared| {
        if (c.sqlite3_prepare_v3(db, text.ptr, -1, c.SQLITE_PREPARE_PERSISTENT, prepared, null) != c.SQLITE_OK)
            return error.DatabasePrepare;
    }
}

fn monotonic() i64 {
    var ts: c.struct_timespec = undefined;
    _ = c.clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return @intCast(ts.tv_sec);
}

fn exec(sql: [*:0]const u8) !void {
    if (c.sqlite3_exec(db, sql, null, null, null) != c.SQLITE_OK) return error.Database;
}

fn begin() !void {
    if (!transaction) {
        try exec("BEGIN IMMEDIATE");
        transaction = true;
        failed = false;
    }
    if (failed or c.sqlite3_get_autocommit(db) != 0) return error.Database;
}

// Called by the event loop before releasing any response that observed this transaction.
pub fn commit() bool {
    const ok = !failed and c.sqlite3_get_autocommit(db) == 0 and c.sqlite3_exec(db, "COMMIT", null, null, null) == c.SQLITE_OK;
    if (!ok) exec("ROLLBACK") catch {};
    transaction = false;
    failed = false;
    return ok;
}

fn stmt(query: Query) ?*c.sqlite3_stmt {
    return statements[@backingInt(query)];
}

fn reset(s: ?*c.sqlite3_stmt) void {
    _ = c.sqlite3_reset(s);
    _ = c.sqlite3_clear_bindings(s);
}

fn bindInt(s: ?*c.sqlite3_stmt, index: c_int, value: i64) !void {
    if (c.sqlite3_bind_int64(s, index, value) != c.SQLITE_OK) return error.Database;
}

fn bindText(s: ?*c.sqlite3_stmt, index: c_int, value: []const u8) !void {
    // SQLITE_STATIC is safe: every statement is stepped and reset within this request.
    if (c.sqlite3_bind_text(s, index, value.ptr, @intCast(value.len), null) != c.SQLITE_OK) return error.Database;
}

fn textColumn(s: ?*c.sqlite3_stmt, index: c_int) []const u8 {
    const ptr = c.sqlite3_column_text(s, index);
    return if (ptr == null) "" else ptr[0..@intCast(c.sqlite3_column_bytes(s, index))];
}

fn append(bytes: []const u8) !void {
    try output.appendSlice(alloc, bytes);
}

fn number(value: i64) !void {
    var buf: [24]u8 = undefined;
    try append(try std.fmt.bufPrint(&buf, "{d}", .{value}));
}

fn quoted(value: []const u8) !void {
    try append("\"");
    var start: usize = 0;
    for (value, 0..) |ch, i| {
        if (ch >= 0x20 and ch != '"' and ch != '\\') continue;
        try append(value[start..i]);
        if (ch == '"' or ch == '\\') {
            try append(&.{ '\\', ch });
        } else {
            const hex = "0123456789abcdef";
            try append(&.{ '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15] });
        }
        start = i + 1;
    }
    try append(value[start..]);
    try append("\"");
}

fn postObject(id: i64, body: []const u8, created: []const u8, author: []const u8, likes: i64) !void {
    try append("{\"id\":");
    try number(id);
    try append(",\"body\":");
    try quoted(body);
    try append(",\"created_at\":");
    try quoted(created);
    try append(",\"author\":");
    try quoted(author);
    try append(",\"like_count\":");
    try number(likes);
    try append("}");
}

fn row(s: ?*c.sqlite3_stmt) !void {
    try postObject(c.sqlite3_column_int64(s, 0), textColumn(s, 1), textColumn(s, 2), textColumn(s, 3), c.sqlite3_column_int64(s, 4));
}

fn response(status: u16) Response {
    return .{ .status = status, .body = output.items };
}

fn err(status: u16, comptime message: []const u8) Response {
    return .{ .status = status, .body = "{\"error\":\"" ++ message ++ "\"}" };
}

fn positiveId(value: []const u8) ?i64 {
    if (value.len == 0) return null;
    for (value) |ch| if (ch < '0' or ch > '9') return null;
    const id = std.fmt.parseInt(i64, value, 10) catch return null;
    return if (id > 0 and id <= 9007199254740991) id else null;
}

fn json(arena: std.mem.Allocator, bytes: []const u8) !std.json.Value {
    if (!std.unicode.utf8ValidateSlice(bytes)) return error.InvalidJson;
    return try std.json.parseFromSliceLeaky(std.json.Value, arena, bytes, .{ .allocate = .alloc_always, .duplicate_field_behavior = .use_last });
}

fn decode(arena: std.mem.Allocator, value: []const u8) ![]u8 {
    const decoder = std.base64.url_safe_no_pad.Decoder;
    const out = try arena.alloc(u8, try decoder.calcSizeForSlice(value));
    try decoder.decode(out, value);
    return out;
}

fn numeric(value: std.json.Value) ?f64 {
    const n: f64 = switch (value) {
        .integer => |i| @floatFromInt(i),
        .float => |f| f,
        .number_string => |s| std.fmt.parseFloat(f64, s) catch return null,
        else => return null,
    };
    return if (std.math.isFinite(n)) n else null;
}

const User = struct { id: i64, username: []const u8 };
fn authenticate(arena: std.mem.Allocator, authorization: []const u8) !User {
    if (!std.mem.startsWith(u8, authorization, "Bearer ")) return error.MissingBearer;
    const token = authorization[7..];
    var parts = std.mem.splitScalar(u8, token, '.');
    const head = parts.next() orelse return error.InvalidToken;
    const payload = parts.next() orelse return error.InvalidToken;
    const sig = parts.next() orelse return error.InvalidToken;
    if (parts.next() != null or head.len == 0 or payload.len == 0) return error.InvalidToken;
    const signature = decode(arena, sig) catch return error.InvalidToken;
    if (signature.len != 32) return error.InvalidToken;
    var expected: [32]u8 = undefined;
    std.crypto.auth.hmac.sha2.HmacSha256.create(&expected, token[0 .. head.len + 1 + payload.len], secret);
    if (!std.crypto.timing_safe.eql([32]u8, expected, signature[0..32].*)) return error.InvalidToken;
    const h = json(arena, decode(arena, head) catch return error.InvalidToken) catch return error.InvalidToken;
    const p = json(arena, decode(arena, payload) catch return error.InvalidToken) catch return error.InvalidToken;
    if (h != .object or p != .object) return error.InvalidToken;
    const alg = h.object.get("alg") orelse return error.InvalidToken;
    if (alg != .string or !std.mem.eql(u8, alg.string, "HS256")) return error.InvalidToken;
    const exp = numeric(p.object.get("exp") orelse return error.InvalidToken) orelse return error.InvalidToken;
    var wall: c.struct_timespec = undefined;
    _ = c.clock_gettime(c.CLOCK_REALTIME, &wall);
    const now = @as(f64, @floatFromInt(wall.tv_sec)) + @as(f64, @floatFromInt(wall.tv_nsec)) / 1e9;
    if (exp <= now) return error.InvalidToken;
    if (p.object.get("nbf")) |nbf| {
        if ((numeric(nbf) orelse return error.InvalidToken) > now) return error.InvalidToken;
    }
    const sub = p.object.get("sub") orelse return error.InvalidPayload;
    const username = p.object.get("username") orelse return error.InvalidPayload;
    if (sub != .string or username != .string) return error.InvalidPayload;
    return .{ .id = positiveId(sub.string) orelse return error.InvalidPayload, .username = username.string };
}

fn whitespace(ch: u21) bool {
    return switch (ch) {
        9...13, 0x20, 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff => true,
        else => false,
    };
}

fn trimmed(value: []const u8) ![]const u8 {
    var iterator = (try std.unicode.Utf8View.init(value)).iterator();
    var start: usize = value.len;
    var end: usize = 0;
    var count: usize = 0;
    var length: usize = 0;
    while (iterator.nextCodepoint()) |ch| {
        if (!whitespace(ch)) {
            if (start == value.len) start = iterator.i - (std.unicode.utf8CodepointSequenceLength(ch) catch unreachable);
            end = iterator.i;
            length = count + 1;
        }
        if (start != value.len) count += 1;
    }
    if (end == 0) return error.EmptyBody;
    if (length > 500) return error.LongBody;
    return value[start..end];
}

pub fn handle(method: []const u8, target: []const u8, authorization: []const u8, body: []const u8) Response {
    output.clearRetainingCapacity();
    return route(method, target, authorization, body) catch {
        // A failed statement may have rolled the transaction back implicitly.
        if (transaction and c.sqlite3_get_autocommit(db) != 0) failed = true;
        return internal;
    };
}

fn route(method: []const u8, target: []const u8, authorization: []const u8, body: []const u8) !Response {
    const path = target[0 .. std.mem.indexOfScalar(u8, target, '?') orelse target.len];
    const get = std.mem.eql(u8, method, "GET");
    const post = std.mem.eql(u8, method, "POST");
    const create = post and std.mem.eql(u8, path, "/posts");
    const prefix = std.mem.startsWith(u8, path, "/posts/");
    const like = post and prefix and path.len >= 12 and std.mem.endsWith(u8, path, "/like");
    if (get and std.mem.eql(u8, path, "/health")) {
        const s = stmt(.health);
        defer reset(s);
        if (c.sqlite3_step(s) != c.SQLITE_ROW)
            return .{ .status = 503, .body = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database query failed\"}" };
        try append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        try number(@max(0, monotonic() - started));
        try append("}");
        return response(200);
    }
    if (get and std.mem.eql(u8, path, "/feed")) {
        const s = stmt(.feed);
        defer reset(s);
        try append("{\"posts\":[");
        var first = true;
        while (true) {
            const rc = c.sqlite3_step(s);
            if (rc == c.SQLITE_DONE) break;
            if (rc != c.SQLITE_ROW) return error.Database;
            if (!first) try append(",");
            try row(s);
            first = false;
        }
        try append("]}");
        return response(200);
    }
    var arena = std.heap.ArenaAllocator.init(alloc);
    defer arena.deinit();
    var user: User = undefined;
    if (create or like) {
        user = authenticate(arena.allocator(), authorization) catch |e| return switch (e) {
            error.MissingBearer => err(401, "missing bearer token"),
            error.InvalidPayload => err(401, "invalid token payload"),
            else => err(401, "invalid or expired token"),
        };
    }
    if (create) {
        const value = json(arena.allocator(), body) catch return err(400, "malformed JSON body");
        if (value != .object) return err(400, "body is required");
        const field = value.object.get("body") orelse return err(400, "body is required");
        if (field != .string) return err(400, "body is required");
        const content = trimmed(field.string) catch |e| return if (e == error.LongBody) err(400, "body must be at most 500 characters") else err(400, "body is required");
        try begin();
        const s = stmt(.create);
        defer reset(s);
        try bindInt(s, 1, user.id);
        try bindText(s, 2, content);
        if (c.sqlite3_step(s) != c.SQLITE_ROW) return error.Database;
        try append("{\"post\":");
        try postObject(c.sqlite3_column_int64(s, 0), content, textColumn(s, 1), user.username, 0);
        try append("}");
        if (c.sqlite3_step(s) != c.SQLITE_DONE) return error.Database;
        return response(201);
    }
    if (prefix and (get or like)) {
        const value = path[7 .. path.len - @as(usize, if (like) 5 else 0)];
        if (std.mem.indexOfScalar(u8, value, '/') != null) return err(404, "not found");
        const id = positiveId(value) orelse return err(400, "invalid post id");
        if (get) {
            const s = stmt(.post);
            defer reset(s);
            try bindInt(s, 1, id);
            const rc = c.sqlite3_step(s);
            if (rc == c.SQLITE_DONE) return err(404, "post not found");
            if (rc != c.SQLITE_ROW) return error.Database;
            try append("{\"post\":");
            try row(s);
            try append("}");
            return response(200);
        }
        try begin();
        const s = stmt(.like);
        defer reset(s);
        try bindInt(s, 1, user.id);
        try bindInt(s, 2, id);
        if (c.sqlite3_step(s) != c.SQLITE_DONE) return error.Database;
        const added = c.sqlite3_changes(db) != 0;
        if (!added) {
            const exists = stmt(.exists);
            defer reset(exists);
            try bindInt(exists, 1, id);
            const rc = c.sqlite3_step(exists);
            if (rc == c.SQLITE_DONE) return err(404, "post not found");
            if (rc != c.SQLITE_ROW) return error.Database;
        }
        try append(if (added) "{\"liked\":true,\"already_liked\":false,\"post_id\":" else "{\"liked\":true,\"already_liked\":true,\"post_id\":");
        try number(id);
        try append("}");
        return response(if (added) 201 else 200);
    }
    return err(404, "not found");
}
