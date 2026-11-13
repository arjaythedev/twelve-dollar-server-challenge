// The $12 Server Challenge's feed API (SPEC.md), written as an application on
// Plod writes it: five routes, a bearer check, and SQL through goose-sqlite.
//
//   GET  /health              {"status":"ok","db":"ok","uptime_s":n}
//   GET  /feed                the 20 newest posts
//   GET  /posts/:id           one post
//   POST /posts               a post by the token's user
//   POST /posts/:id/like      the token's user likes the post
//
// Every query runs in the route, on the worker: SQLite answers from its page
// cache, so a query is CPU work, which a lane would only move to another
// thread on the same core. Each write commits on its own, in WAL with
// synchronous=NORMAL, before the route answers.
import std;
import os;
import json;
import http;
import plod;
import plod.jwt;
import sqlite;

struct St {}

// The worker state: its connection, and the key tokens are signed with.
struct App {
    db: sqlite::Conn&,
    keys: const plod::JwtKey[:],
    started_ns: i64,
}

// The post object's columns, in the order post writes them.
let FEED = """
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
      FROM posts p JOIN users u ON u.id = p.user_id
     ORDER BY p.created_at DESC, p.id DESC LIMIT 20
    """;
// The author by a subquery, not a join: with the join, SQLite's constant
// propagation reads the bound id while planning, and re-prepares the
// statement at every new id.
let ONE_POST = """
    SELECT p.id, p.body, p.created_at,
           (SELECT u.username FROM users u WHERE u.id = p.user_id),
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
      FROM posts p
     WHERE p.id = ?
    """;
let INSERT_POST = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";
// Inserts the like only where the post exists; a like that changes nothing
// is a repeat, or a post that isn't there.
let INSERT_LIKE = """
    INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ?
        ON CONFLICT (user_id, post_id) DO NOTHING
    """;
let POST_EXISTS = "SELECT 1 FROM posts WHERE id = ?";

fn handle(cx) {
    plod::routes(cx) { cx =>
        plod::get(cx, "/health") { health(cx); };
        plod::get(cx, "/feed") { feed(cx); };
        plod::get(cx, "/posts/:id") { show(cx, post_id(cx)); };
        plod::post(cx, "/posts") {
            author(cx) { user, name => create(cx, user, name); };
        };
        plod::post(cx, "/posts/:id/like") {
            author(cx) { user, name => like(cx, user, post_id(cx)); };
        };
    };
}

fn health(cx) {
    var ok = false;
    let failed = sqlite::run() { sqlite::query(cx.app.db, "SELECT 1") { r => ok = sqlite::int(r, 0) == 1; }; };
    var out: u8[>..] = [];
    for e in failed {
        out.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
        http::json_string(out,e.message);
        out.push('}');
        plod::json_bytes(cx, 503, out);
        return;
    }
    guard ok else { plod::fail(cx, 500, "internal server error"); }
    out.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    format(out, (time_ns() - cx.app.started_ns) / 1000000000);
    out.push('}');
    plod::json_bytes(cx, 200, out);
}

fn feed(cx) {
    var out: u8[>..] = [];
    out.append("{\"posts\":[");
    db(cx) {
        sqlite::query(cx.app.db, FEED) { r =>
            if out.len > 10 { out.push(','); }
            post(out, r);
        };
    };
    out.append("]}");
    plod::json_bytes(cx, 200, out);
}

fn show(cx, id: i64) {
    var out: u8[>..] = [];
    out.append("{\"post\":");
    var found = 0;
    db(cx) { found = sqlite::query(cx.app.db, ONE_POST, id) { r => post(out, r); }; };
    guard found > 0 else { plod::fail(cx, 404, "post not found"); }
    out.push('}');
    plod::json_bytes(cx, 200, out);
}

fn create(cx, user: i64, name: const u8[:]) {
    var text: u8[>..] = [];
    post_body(cx, text);
    var out: u8[>..] = [];
    db(cx) {
        sqlite::query(cx.app.db, INSERT_POST, user, text) { r =>
            out.append("{\"post\":{\"id\":");
            format(out, sqlite::int(r, 0));
            out.append(",\"body\":");
            http::json_string(out,text);
            out.append(",\"created_at\":");
            http::json_string(out,sqlite::text(r, 1));
            out.append(",\"author\":");
            http::json_string(out,name);
            out.append(",\"like_count\":0}}");
        };
    };
    plod::json_bytes(cx, 201, out);
}

fn like(cx, user: i64, id: i64) {
    var added = 0;
    var exists = 0;
    db(cx) {
        added = sqlite::exec(cx.app.db, INSERT_LIKE, user, id);
        if added == 0 { exists = sqlite::query(cx.app.db, POST_EXISTS, id) { r => }; }
    };
    guard added > 0 || exists > 0 else { plod::fail(cx, 404, "post not found"); }
    var out: u8[>..] = [];
    out.append("{\"liked\":true,\"already_liked\":");
    out.append(if added > 0 { "false" } else { "true" });
    out.append(",\"post_id\":");
    format(out, id);
    out.push('}');
    plod::json_bytes(cx, if added > 0 { 201 } else { 200 }, out);
}

// A row of POST's columns as a post object.
fn post(out: u8[>..]&, r) {
    out.append("{\"id\":");
    format(out, sqlite::int(r, 0));
    out.append(",\"body\":");
    http::json_string(out,sqlite::text(r, 1));
    out.append(",\"created_at\":");
    http::json_string(out,sqlite::text(r, 2));
    out.append(",\"author\":");
    http::json_string(out,sqlite::text(r, 3));
    out.append(",\"like_count\":");
    format(out, sqlite::int(r, 4));
    out.push('}');
}

// Runs F's queries, and answers 500 when SQLite fails one.
inline fn db<F>(cx) {
    let failed = sqlite::run() { F(); };
    guard failed.len == 0 else { plod::fail(cx, 500, "internal server error"); }
}

// The path's :id: a positive integer, or the route fails with 400.
fn post_id(cx) -> i64 {
    let s = plod::param(cx, "id");
    var id = 0;
    var ok = s.len > 0 && s.len <= 18;
    for c in s {
        ok = ok && c >= '0' && c <= '9';
        id = id * 10 + (c - '0');
    }
    guard ok && id > 0 else { plod::fail(cx, 400, "invalid post id"); }
    return id;
}

// Runs F with the bearer token's user id and username, or fails the route
// with 401.
inline fn author<F>(cx) {
    let auth, has = plod::header(cx, "Authorization");
    guard has && auth.len > 7 && auth[..7] == "Bearer " else { plod::fail(cx, 401, "missing bearer token"); }
    let e = plod::jwt_verify(auth[7..], cx.app.keys, plod::JwtCheck { leeway: 0 }) { t =>
        let user = positive(t.sub);
        var scratch: u8[>..] = [];
        var r = json::reader(t.payload);
        var name = r.text[..0];
        var named = false;
        json::each_member(r) { key =>
            if key == "username" && json::peek(r) == '"' {
                name = json::read_string(r, scratch);
                named = true;
            } else {
                if key == "username" { named = false; }
                json::skip(r);
            }
        };
        guard user > 0 && named else { plod::fail(cx, 401, "invalid token payload"); }
        // jwt_verify checks exp when a token has one; SPEC.md requires it.
        guard t.exp >= 0 else { plod::fail(cx, 401, "invalid or expired token"); }
        F(user, name);
    };
    guard e == plod::JwtError.None else { plod::fail(cx, 401, "invalid or expired token"); }
}

// s as a positive integer, or 0.
fn positive(s: const u8[:]) -> i64 {
    guard s.len > 0 && s.len <= 18 else { return 0; }
    var n = 0;
    for c in s {
        guard c >= '0' && c <= '9' else { return 0; }
        n = n * 10 + (c - '0');
    }
    return n;
}

// Appends the request's body member, trimmed, to out, or fails the route
// with 400.
fn post_body(cx, out: u8[>..]&) {
    var scratch: u8[>..] = [];
    var r = json::reader(plod::body(cx));
    var text = nothing(r);
    var given = false;
    if json::peek(r) == '{' {
        json::each_member(r) { key =>
            if key == "body" && json::peek(r) == '"' {
                text = json::read_string(r, scratch);
                given = true;
            } else {
                if key == "body" { given = false; }
                json::skip(r);
            }
        };
    } else {
        json::skip(r);
    }
    guard json::finish(r) else { plod::fail(cx, 400, "malformed JSON body"); }
    let t = trim(text);
    guard given && t.len > 0 else { plod::fail(cx, 400, "body is required"); }
    guard characters(t) <= 500 else { plod::fail(cx, 400, "body must be at most 500 characters"); }
    out.append(t);
}

// An empty slice rooted at r, which a string r reads may replace.
fn nothing(r: json::Reader&) -> const u8[:] { return r.text[..0]; }

// The characters of UTF-8 text: its bytes that don't continue one.
fn characters(s: const u8[:]) -> i64 {
    var n = 0;
    for c in s { if (c & 0xC0) != 0x80 { n++; } }
    return n;
}

// Every error as SPEC.md writes it, {"error":"<message>"}: the routes fail
// with the message itself; the router's own answers have codes.
fn plod::error_body(body: u8[>..]&, st: const St&, status: i32, code: const u8[:],
                    detail: const u8[:], at: i64) {
    body.append("{\"error\":");
    if code == "not_found" { http::json_string(body,"not found"); }
    else if status >= 500 { http::json_string(body,"internal server error"); }
    else { http::json_string(body,code); }
    body.push('}');
}
