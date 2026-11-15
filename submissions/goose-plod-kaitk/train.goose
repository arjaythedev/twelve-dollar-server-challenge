// The training run for build.sh's profile-guided build of SQLite: app.goose's
// routes, served in-process, on a database shaped like the seed that it
// builds at SQLITE_PATH, for requests in the mix of the challenge's
// bench/load.js. Only SQLite is instrumented, so only its profile is kept.
// The packages' tags are server.goose's: another tag of goose-sqlite would
// profile another sqlite3.c.
import std;
import os;
import json;
import http from "github.com/kaitk/goose-http@v0.5.20";
import hash from "github.com/kaitk/goose-hash@v0.0.4";
import plod from "github.com/kaitk/plod@v0.5.13";
import plod.jwt;
import plod.testing;
import sqlite from "github.com/kaitk/goose-sqlite@v0.0.4";
import app;

// A tenth of the seed's users and posts, and a fifth of its likes.
let USERS = 5000;
let POSTS = 50000;
let LIKES = 400000;
// A user's loop: the feed, one of its posts, a like at 15%, a new post at 2%.
let LOOPS = 40000;
let OPEN_URI: i32 = 64;

let SCHEMA = """
    CREATE TABLE users (
      id          INTEGER PRIMARY KEY,
      username    TEXT NOT NULL UNIQUE,
      created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
    );
    CREATE TABLE posts (
      id          INTEGER PRIMARY KEY,
      user_id     INTEGER NOT NULL REFERENCES users(id),
      body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
      created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
    );
    CREATE TABLE likes (
      user_id     INTEGER NOT NULL REFERENCES users(id),
      post_id     INTEGER NOT NULL REFERENCES posts(id),
      created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
      PRIMARY KEY (user_id, post_id)
    );
    CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
    CREATE INDEX posts_user_id_idx       ON posts (user_id);
    CREATE INDEX likes_post_id_idx       ON likes (post_id);
    """;

// SQLITE_PATH as server.goose opens it.
fn database() -> u8[>..] {
    var path: u8[>..] = [];
    guard env("SQLITE_PATH", path) else { abort("SQLITE_PATH is not set"); }
    var uri: u8[>..] = [];
    uri.append("file:");
    uri.append(path);
    uri.append("?vfs=unix-excl");
    return uri;
}

// Users, posts in id order a minute apart, as the seed's are, and likes.
fn seed(r: rng&) {
    var path: u8[>..] = [];
    guard env("SQLITE_PATH", path) else { abort("SQLITE_PATH is not set"); }
    var db = sqlite::Conn { db: sqlite::Db {}, .. };
    let failed = sqlite::run() {
        db.db = sqlite::open(path);
        sqlite::exec(db.db, "PRAGMA journal_mode = WAL");
        sqlite::exec(db.db, SCHEMA);
        sqlite::exec(db.db, "BEGIN");
        for i in USERS {
            var name: u8[>..] = [];
            name.append("user_");
            format(name, i + 1);
            sqlite::exec(db, "INSERT INTO users (id, username) VALUES (?, ?)", i + 1, name);
        }
        var body: u8[>..] = [];
        for i in POSTS {
            body.resize(0);
            let n = r.rand_range(10, 140);
            for k in n { body.push(if r.rand_range(6) == 0 { ' ' } else { ('a' + r.rand_range(26)) as u8 }); }
            sqlite::exec(db, """
                INSERT INTO posts (id, user_id, body, created_at)
                VALUES (?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 1735689600 + ? * 60, 'unixepoch'))
                """, i + 1, r.rand_range(1, USERS), body, i);
        }
        for i in LIKES {
            sqlite::exec(db, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?, ?)",
                         r.rand_range(1, USERS), r.rand_range(1, POSTS));
        }
        sqlite::exec(db.db, "COMMIT");
    };
    for e in failed { abort(str("train: ", e.message)); }
    sqlite::close(db);
}

thread_fn checkpointer(box: http::Box) {
    var db = sqlite::Db {};
    let failed = sqlite::run() {
        db = sqlite::open(database(), sqlite::READ_WRITE | sqlite::NO_MUTEX | OPEN_URI);
        sqlite::exec(db, """
            PRAGMA synchronous = NORMAL;
            PRAGMA wal_autocheckpoint = 0;
            PRAGMA cache_size = 16;
            """);
        sqlite::busy_timeout(db, 1000);
    };
    for e in failed { abort(str("train: ", e.message)); }
    http::lane(box) { tag, bytes =>
        let ran = sqlite::run() { sqlite::exec(db, "PRAGMA wal_checkpoint(PASSIVE)"); };
        for e in ran { abort(str("train: ", e.message)); }
    };
    sqlite::close(db);
}

// A write's WAKE at the pass's end, which COMMITs, and before it, for the
// write that begins a hold, the lane's reply that ends it.
let WAKES = [plod::Wake { .. }, plod::Wake { .. }];

// Serves text, and ends the run unless the answer is a success.
fn serve(a: App&, text: const u8[:]) {
    let res = plod::serve_one(text, a, St {}, WAKES) { cx => handle(cx); };
    guard res.len > 10 && res[..10] == "HTTP/1.1 2" else { abort(str("train: ", text, "\n", res)); }
}

// A POST to path with body, as user, whose token keys signs.
fn post(a: App&, path: const u8[:], body: const u8[:], user: i64) {
    var name: u8[>..] = [];
    name.append("user_");
    format(name, user);
    var sub: u8[>..] = [];
    format(sub, user);
    var req: u8[>..] = [];
    req.append("POST ");
    req.append(path);
    req.append(" HTTP/1.1\r\nHost: train\r\nContent-Type: application/json\r\nAuthorization: Bearer ");
    plod::jwt_sign(req, a.keys[0], plod::Jwt { sub: sub }, 3600) { p =>
        json::format_key(p, "username");
        json::format_string(p, name);
    };
    req.append("\r\nContent-Length: ");
    format(req, body.len);
    req.append("\r\n\r\n");
    req.append(body);
    serve(a, req);
}

fn main() {
    var r = rng { 20261114 };
    seed(r);
    let checkpoints = http::lane_open(http::LaneConfig { slots: 4, .. }, checkpointer);
    var db = sqlite::Conn { db: sqlite::Db {}, .. };
    let failed = sqlite::run() {
        db.db = sqlite::open(database(), sqlite::READ_WRITE | sqlite::NO_MUTEX | OPEN_URI, false);
        sqlite::exec(db.db, """
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA cache_size = -16000;
            PRAGMA wal_autocheckpoint = 65536;
            """);
        sqlite::busy_timeout(db.db, 5000);
    };
    for e in failed { abort(str("train: ", e.message)); }
    let keys = [plod::JwtKey { secret: "training" }];
    var a = App { db: db, keys: keys, started_ns: time_ns(), checkpoints: checkpoints };
    var newest = POSTS;
    var req: u8[>..] = [];
    var path: u8[>..] = [];
    for i in LOOPS {
        serve(a, "GET /feed HTTP/1.1\r\nHost: train\r\n\r\n");
        let id = newest - r.rand_range(20);
        req.resize(0);
        req.append("GET /posts/");
        format(req, id);
        req.append(" HTTP/1.1\r\nHost: train\r\n\r\n");
        serve(a, req);
        let user = r.rand_range(1, USERS);
        if r.rand_range(100) < 15 {
            path.resize(0);
            path.append("/posts/");
            format(path, id);
            path.append("/like");
            post(a, path, "", user);
        }
        if r.rand_range(100) < 2 {
            post(a, "/posts", "{\"body\":\"a new post from the training run\"}", user);
            newest++;
        }
    }
    sqlite::close(db);
    http::lane_stop(checkpoints);
}
