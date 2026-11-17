// Effectful handlers. Pure parsing/authentication receive all dependencies as values.
import src.db;
import src.http;
import src.jwt;
import src.json;
import src.native;
namespace app;

var signing_key: jwt::Key;
var token_header: u8[>..];
var token_payload: u8[>..];
var key_scratch: u8[>..];
var value_scratch: u8[>..];
var username: u8[>..];
var content: u8[>..];
var created: u8[>..];

fn initialize(secret: const u8[:]) { signing_key = jwt::key(secret); }

fn error(out: u8[>..]&, status: i32, message: const u8[:]) -> i32 {
    out.clear();
    out.append("{\"error\":\"");
    json::esc(out, message);
    out.append("\"}");
    return status;
}

fn internal(out: u8[>..]&) -> i32 { return error(out, 500, "internal server error"); }

fn authenticate(bytes: const u8[:], req: model::Request&, now: f64, want_name: bool) -> model::Auth {
    username.clear();
    let auth = if req.has_auth { bytes[req.auth.start..req.auth.end] } else { "" };
    return jwt::verify(signing_key, auth, now, want_name,
        token_header, token_payload, key_scratch, value_scratch, username);
}

fn auth_error(result: model::Auth&, out: u8[>..]&) -> i32 {
    return match result {
        Missing => error(out, 401, "missing bearer token"),
        Invalid => error(out, 401, "invalid or expired token"),
        InvalidClaims => error(out, 401, "invalid token payload"),
        Verified => { assert(false); 500 },
    };
}

// Last duplicate key wins; malformed JSON and a non-string/missing body stay distinct.
fn create(bytes: const u8[:], req: model::Request&, now: f64, out: u8[>..]&) -> i32 {
    var auth = authenticate(bytes, req, now, true);
    var uid: i64 = 0;
    match auth {
        Verified claims => { uid = claims.user_id; },
        Missing, Invalid, InvalidClaims => { return auth_error(auth, out); },
    }
    let raw = bytes[req.body.start..req.body.end];
    var cursor = 0;
    var start = -1;
    var end = -1;
    var escaped = false;
    let result = json::object_members(raw, cursor, key_scratch) { key =>
        if key == "body" && cursor < raw.len && raw[cursor] == '"' {
            let a, b, esc, valid = json::read_string(raw, cursor);
            if !valid { return -1 from json::object_members; }
            start = a; end = b; escaped = esc;
        } else {
            if !json::skip_value(raw, cursor) { return -1 from json::object_members; }
            if key == "body" { start = -1; }
        }
    };
    if result < 0 { return error(out, 400, "malformed JSON body"); }
    if result == 0 || start < 0 { return error(out, 400, "body is required"); }
    content.clear();
    if escaped { json::unescape(raw[start..end], content); }
    else { content.append(raw[start..end]); }
    let a, b, count = json::trim_count(content);
    if count < 0 { return error(out, 400, "malformed JSON body"); }
    if count == 0 { return error(out, 400, "body is required"); }
    if count > 500 { return error(out, 400, "body must be at most 500 characters"); }
    if native::begin() != 0 { return internal(out); }
    created.clear();
    var id: i64 = 0;
    if native::add_post(uid, content[a..b], id, created) != 0 { return internal(out); }
    assert(id > 0);
    out.append("{\"post\":");
    db::post_json(out, id, content[a..b], created, username, 0);
    out.push('}');
    return 201;
}

fn like(bytes: const u8[:], req: model::Request&, raw_id: model::ByteRange,
        now: f64, out: u8[>..]&) -> i32 {
    var auth = authenticate(bytes, req, now, false);
    var uid: i64 = 0;
    match auth {
        Verified claims => { uid = claims.user_id; },
        Missing, Invalid, InvalidClaims => { return auth_error(auth, out); },
    }
    let id = model::positive_int(bytes[raw_id.start..raw_id.end]);
    if id <= 0 { return error(out, 400, "invalid post id"); }
    let result = db::like(uid, id);
    var status: i32 = 200;
    match result {
        Missing => { return error(out, 404, "post not found"); },
        Failed => { return internal(out); },
        Created => { out.append("{\"liked\":true,\"already_liked\":false,\"post_id\":"); status = 201; },
        Existing => { out.append("{\"liked\":true,\"already_liked\":true,\"post_id\":"); },
    }
    json::push_int(out, id);
    out.push('}');
    return status;
}

// JSON only. The transport constructs headers and defers these bytes until COMMIT.
fn respond(bytes: const u8[:], req: model::Request&, started_us: i64, out: u8[>..]&) -> i32 {
    out.clear();
    let route = http::route(bytes, req);
    match route {
        Missing => { return error(out, 404, "not found"); },
        InvalidId => { return error(out, 400, "invalid post id"); },
        Create => { return create(bytes, req, native::now(), out); },
        Like value => { return like(bytes, req, value.raw_id, native::now(), out); },
        Health => {
            let uptime = (native::micros() - started_us) / 1000000;
            value_scratch.clear();
            if native::begin() != 0 || native::ping(value_scratch) != 0 {
                out.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                json::esc(out, value_scratch);
                out.append("\"}");
                return 503;
            }
            out.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            json::push_int(out, uptime);
            out.push('}');
            return 200;
        },
        Feed => {
            if !db::feed(out) { return internal(out); }
            return 200;
        },
        Read value => {
            let result = db::post(value.id, out);
            return match result {
                Found => 200,
                Missing => error(out, 404, "post not found"),
                Failed => internal(out),
            };
        },
    }
}
