// Profile training, built only by build.sh's instrumented pass. It serves a request mix from memory against a
// synthetic database it creates (no challenge data), so GCC's profile of SQLite and src/btree.c sees the server's
// hot paths: B-tree reads, the SQL writes, group commits. Not part of bin/server.
const std = @import("std");
const m = @import("main.zig");
const c = m.c;

const tok = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiI3IiwidXNlcm5hbWUiOiJ0cmFpbmVyIiwiZXhwIjo0MTAyNDQ0ODAwfQ.fmsWbbPWXeDx0P3cbcdmOTM6bC10yq05y3vDN36tSfU";
const schema =
    "PRAGMA journal_mode=WAL;" ++
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));" ++
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));" ++
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));" ++
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC); CREATE INDEX posts_user_id_idx ON posts (user_id); CREATE INDEX likes_post_id_idx ON likes (post_id);" ++
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO users (id, username) SELECT i, 'user' || i FROM n;" ++
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) INSERT INTO posts (user_id, body, created_at)" ++
    "  SELECT 1 + i % 20000, 'synthetic post ' || i || ' \"quoted\" text', strftime('%Y-%m-%dT%H:%M:%fZ', 1700000000 + i * 60, 'unixepoch') FROM n;" ++
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 600000) INSERT OR IGNORE INTO likes (user_id, post_id)" ++
    "  SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 200000 FROM n;";

pub fn main() void {
    const path = std.posix.getenv("PGO_TRAIN_DB") orelse std.process.exit(1);
    _ = c.sqlite3_initialize();
    _ = std.c.unlink(path);
    var d: ?*c.sqlite3 = null;
    if (c.sqlite3_open(path, &d) != c.SQLITE_OK or c.sqlite3_exec(d, schema, null, null, null) != c.SQLITE_OK) std.process.exit(1);
    _ = c.sqlite3_close(d);
    m.setup(path, "pgo-training-secret");
    var req: [1024]u8 = undefined;
    var i: u32 = 1;
    while (i <= 300000) : (i += 1) {
        const k = i % 217;
        const id = 1 + (i *% 7919) % 200000;
        const r = if (k < 100)
            std.fmt.bufPrint(&req, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n", .{})
        else if (k < 200)
            std.fmt.bufPrint(&req, "GET /posts/{d} HTTP/1.1\r\nHost: x\r\n\r\n", .{id})
        else if (k < 215)
            std.fmt.bufPrint(&req, "POST /posts/{d}/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {s}\r\nContent-Length: 0\r\n\r\n", .{ id, tok })
        else blk: {
            var b: [64]u8 = undefined;
            const body = std.fmt.bufPrint(&b, "{{\"body\":\"training post {d}\"}}", .{i}) catch unreachable;
            break :blk std.fmt.bufPrint(&req, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {s}\r\nContent-Type: application/json\r\nContent-Length: {d}\r\n\r\n{s}", .{ tok, body.len, body });
        };
        m.trainServe(r catch unreachable);
        if (i % 64 == 0) m.trainFlush();
    }
    std.process.exit(0);
}
