// Application policy for the pinned Pavlyhalim submission. Input slices are
// borrowed for this call. SQLite and asynchronous response bytes stay native.
struct WireRequest {
    status: i32, method: u32,
    path_start: u32, path_len: u32, auth_start: u32, auth_len: u32,
    body_start: u32, body_len: u32, total: u32, keep_alive: bool,
}
struct Output { arena: u32, offset: u32, len: u32, error: u8, database: bool }
struct Auth { uid: u64, name_len: u32, error: u8 }
struct Body { start: u32, len: u32, error: u8, escaped: bool }
struct Answer { output: Output, held: bool }

const NOT_FOUND: u8 = 1;
const BAD_ID: u8 = 2;
const NO_POST: u8 = 3;
const MISSING_AUTH: u8 = 4;
const INVALID_AUTH: u8 = 5;
const BAD_CLAIMS: u8 = 6;
const BAD_JSON: u8 = 7;
const NO_BODY: u8 = 8;
const LONG_BODY: u8 = 9;
const INTERNAL: u8 = 10;
const BAD_HTTP: u8 = 11;

extern fn g300_native_parse(input: const u8[:]) -> WireRequest;
extern fn g300_native_auth(auth: const u8[:], name: u8[:]) -> Auth;
extern fn g300_native_body(body: const u8[:]) -> Body;
extern fn g300_native_unescape(raw: const u8[:], out: u8[:]) -> i64;
extern fn g300_native_response(kind: u32, id: u64, uid: u64,
                               name: const u8[:], text: const u8[:], close: bool) -> Output;

fn error_answer(code: u8) -> Answer {
    return Answer { output: Output { error: code, .. }, held: false };
}
fn prefix(text: const u8[:], head: const u8[:]) -> bool {
    return text.len >= head.len && text[..head.len] == head;
}
fn post_id(text: const u8[:]) -> u64 {
    if text.len == 0 || text.len > 18 || text[0] == '0' { return 0; }
    var id: u64 = 0;
    for c in text {
        if c < '0' || c > '9' { return 0; }
        id = id * 10 + (c - '0');
    }
    return id;
}
fn whitespace(c: u8) -> bool { return c == 32 || (c >= 9 && c <= 13); }

fn create_answer(uid: u64, name: const u8[:], text: const u8[:], close: bool) -> Answer {
    // Preserve the reference's decode limit and ASCII trimming exactly.
    if text.len > 4096 { return error_answer(LONG_BODY); }
    var lo: i64 = 0;
    var hi = text.len;
    while lo < hi && whitespace(text[lo]) { lo += 1; }
    while hi > lo && whitespace(text[hi - 1]) { hi -= 1; }
    if lo == hi { return error_answer(NO_BODY); }
    var chars: i64 = 0;
    for c in text[lo..hi] { if (c & 0xC0) != 0x80 { chars += 1; } }
    if chars > 500 { return error_answer(LONG_BODY); }
    return Answer { output: g300_native_response(5, 0, uid, name, text[lo..hi], close), held: false };
}

fn application(input: const u8[:], r: WireRequest, writes_ok: bool) -> Answer {
    let path = input[r.path_start..r.path_start + r.path_len];
    let close = !r.keep_alive;
    if r.method == 1 {
        if path == "/feed" {
            return Answer { output: g300_native_response(2, 0, 0, "", "", close), held: false };
        }
        if path.len > 7 && prefix(path, "/posts/") {
            let id = post_id(path[7..]);
            if id == 0 { return error_answer(BAD_ID); }
            return Answer { output: g300_native_response(3, id, 0, "", "", close), held: false };
        }
        if path == "/health" {
            return Answer { output: g300_native_response(1, 0, 0, "", "", close), held: false };
        }
    }
    if r.method == 2 {
        let create = path == "/posts";
        let like = path.len > 12 && prefix(path, "/posts/") && path[path.len - 5..] == "/like";
        if !create && !like { return error_answer(NOT_FOUND); }
        if !writes_ok { return Answer { output: Output { .. }, held: true }; }
        // Request-scoped scratch dies after SQLite consumes it. Unescaped body
        // text remains a slice of the receive buffer, with no DTO or text copy.
        if like {
            var unused: u8[0] = [];
            let auth = g300_native_auth(input[r.auth_start..r.auth_start + r.auth_len], unused);
            if auth.error != 0 { return error_answer(auth.error); }
            let id = post_id(path[7..path.len - 5]);
            if id == 0 { return error_answer(BAD_ID); }
            return Answer { output: g300_native_response(4, id, auth.uid, "", "", close), held: false };
        }
        var name = default<u8[255]>();
        let auth = g300_native_auth(input[r.auth_start..r.auth_start + r.auth_len], name);
        if auth.error != 0 { return error_answer(auth.error); }
        let body = input[r.body_start..r.body_start + r.body_len];
        let field = g300_native_body(body);
        if field.error != 0 { return error_answer(field.error); }
        let raw = body[field.start..field.start + field.len];
        if !field.escaped { return create_answer(auth.uid, name[..auth.name_len], raw, close); }
        var decoded = default<u8[4096]>();
        let len = g300_native_unescape(raw, decoded);
        if len < 0 { return error_answer(LONG_BODY); }
        return create_answer(auth.uid, name[..auth.name_len], decoded[..len], close);
    }
    return error_answer(NOT_FOUND);
}
