// The $12 server in Goose: every request is Goose, from the first byte to the last.
//
// Goose owns the pass (what to wait for, when to commit, when writes wait out a checkpoint),
// HTTP framing and parsing, routing, HS256 (SHA-256 and HMAC are written here), JSON reading
// and writing, the body's Unicode trim and character count, the decoding of SQLite's record
// format, the feed and post read plans over SQLite's B-tree cursors, and every response byte.
// C (net.c, db.c) owns only what Goose cannot touch: io_uring and its buffers, and SQLite.
import std;

// ---------------------------------------------------------------------------------------------
// C: the transport (net.c)

struct NetEv { conn: i64, kind: i64 }

extern fn net_init(host: const u8[:], port: i64) -> i64;
extern fn net_wait();
extern fn net_next(ev: NetEv&, data: u8[>..]&) -> bool;
extern fn net_reply(conn: i64, head: const u8[:], body: const u8[:]);
extern fn net_keep(conn: i64, rest: const u8[:]);
extern fn net_hold(conn: i64, rest: const u8[:]);
extern fn net_close(conn: i64);
extern fn net_held() -> i64;
extern fn net_release();
extern fn net_flush(committed: bool);

// C: SQLite (db.c)

struct SqlRow { id: i64, likes: i64, blen: i64, clen: i64, alen: i64 }

extern fn db_open(path: const u8[:], err: u8[>..]&) -> i32;
extern fn db_begin() -> i32;
extern fn db_commit() -> bool;
extern fn db_tick();
extern fn db_writes_held() -> bool;
extern fn bt_seek(cur: i32, rowid: i64) -> i32;
extern fn bt_seek_ge(cur: i32, key: i64) -> i32;
extern fn bt_first(cur: i32) -> i32;
extern fn bt_next(cur: i32) -> i32;
extern fn bt_payload(cur: i32, out: u8[>..]&) -> i64;
extern fn bt_count_likes(lo: i64, hi: i64, n: u32[:]) -> i32;
extern fn db_insert_post(user_id: i64, body: const u8[:], id: i64&, created: u8[>..]&) -> i32;
extern fn db_insert_like(user_id: i64, post_id: i64) -> i32;
extern fn db_health(err: u8[>..]&) -> i32;
extern fn db_feed_sql_step(row: SqlRow&, text: u8[>..]&) -> i32;
extern fn env_get(name: const u8[:], out: u8[>..]&) -> bool;
extern fn now_unix() -> f64;
extern fn now_mono_ms() -> i64;
extern fn log_line(s: const u8[:]);

// The cursors db.c opens for each pass.
let C_POSTS: i32 = 0;
let C_USERS: i32 = 1;
let C_FEED: i32 = 2;
let C_LIKES: i32 = 3;

let FEED_LEN = 20;
// Count likes with one index range walk when the feed's ids span at most this many.
let RANGE_SPAN = 512;
let MAX_HEAD = 16384;
let MAX_BODY = 1048576;
let MAX_CHARS = 500;

// ---------------------------------------------------------------------------------------------
// Process state. Scratch buffers are reused from request to request as storage only: each is
// cleared before use, and nothing read from the database survives the request that read it.

var head: u8[>..];      // a response's status line and headers
var body: u8[>..];      // a response's body
var text: u8[>..];      // record bytes copied out of SQLite for the request being served
var scratch: u8[>..];   // decoded JSON strings
var scratch2: u8[>..];  // a second one, for the token payload's claims
var jwt_head: u8[>..];  // the token's decoded header and payload
var jwt_body: u8[>..];

var secret_inner: u32[8];   // SHA-256 state after the HMAC key's inner and outer pad blocks:
var secret_outer: u32[8];   // the key schedule, computed once (not a verification result)
var started_ms: i64;

struct Row { id: i64, uid: i64, b0: i64, b1: i64, c0: i64, c1: i64, a0: i64, a1: i64, likes: i64 }
var rows: Row[20];
var order: i64[20];
var nrows: i64;
var like_counts: u32[513];

// ---------------------------------------------------------------------------------------------
// The pass

fn main() {
    if !startup() { exit(1); }
    var data: u8[>..] = [];
    var ev = NetEv { conn: 0, kind: 0 };
    loop {
        net_wait();
        data.clear();
        while net_next(ev, data) {
            serve(ev.conn, data);
            data.clear();
        }
        // A checkpoint hold that just ended: serve the writes it held, in this pass.
        if net_held() > 0 && !db_writes_held() {
            net_release();
            while net_next(ev, data) {
                serve(ev.conn, data);
                data.clear();
            }
        }
        // No response from this pass reaches the kernel before its COMMIT returns.
        let committed = db_commit();
        net_flush(committed);
        db_tick();
    }
}

fn startup() -> bool {
    var path: u8[>..] = [];
    var host: u8[>..] = [];
    var port_s: u8[>..] = [];
    var secret: u8[>..] = [];
    if !env_get("SQLITE_PATH", path) { path.append("feed.db"); }
    if !env_get("HOST", host) { host.append("127.0.0.1"); }
    if !env_get("PORT", port_s) { port_s.append("3000"); }
    env_get("JWT_SECRET", secret);
    let port, port_ok = parse_int(port_s);
    if !port_ok || port <= 0 || port > 65535 { log_line("PORT must be 1-65535"); return false; }
    var err: u8[>..] = [];
    if db_open(path, err) != 0 {
        var m: u8[>..] = [];
        m.append("database: ");
        m.append(err);
        log_line(m);
        return false;
    }
    hmac_key(secret);
    started_ms = now_mono_ms();
    let rc = net_init(host, port);
    if rc != 0 {
        var m: u8[>..] = [];
        format(m, "listen ", host, ":", port, " failed (errno ", -rc, ")");
        log_line(m);
        return false;
    }
    return true;
}

// Parses and answers every complete request in `data` (the connection's earlier partial bytes
// come first). During a checkpoint hold a write stops it: that request and everything behind it
// stay with the connection until the hold ends.
fn serve(conn: i64, data: const u8[:]) {
    var off = 0;
    var req = Req { .. };
    while off < data.len {
        let rest = data[off..];
        let st = parse_request(rest, req);
        if st == 0 { break; }
        if st < 0 {
            error(conn, 400, "bad request");
            net_close(conn);
            return;
        }
        if req.post && db_writes_held() {
            net_hold(conn, rest);
            return;
        }
        handle(conn, rest, req);
        if req.close { net_close(conn); return; }
        off += req.used;
    }
    if off < data.len { net_keep(conn, data[off..]); }
}

// ---------------------------------------------------------------------------------------------
// HTTP/1.1 framing. Only what the API uses is looked at: the request line, Authorization,
// Content-Length, Connection and Transfer-Encoding. Offsets are into the request's bytes.

struct Req {
    post: bool, get: bool, close: bool, has_auth: bool,
    p0: i64, p1: i64,      // path, without the query
    a0: i64, a1: i64,      // Authorization value, trimmed
    b0: i64, b1: i64,      // body
    used: i64,
}

let NL = byte_set("\n");

fn trim_sp(b: const u8[:], s: i64, e: i64) -> i64, i64 {
    var i = s;
    var j = e;
    while i < j && (b[i] == ' ' || b[i] == '\t') { i++; }
    while j > i && (b[j - 1] == ' ' || b[j - 1] == '\t') { j--; }
    return i, j;
}

// ASCII case-insensitive match against a lower-case name.
fn name_is(b: const u8[:], s: i64, e: i64, lower: const u8[:]) -> bool {
    if e - s != lower.len { return false; }
    for k in lower.len {
        var c = b[s + k];
        if c >= 'A' && c <= 'Z' { c += 32; }
        if c != lower[k] { return false; }
    }
    return true;
}

// 1 a whole request (r filled, r.used its length), 0 need more bytes, -1 malformed.
fn parse_request(b: const u8[:], r: Req&) -> i32 {
    let nl = find_any(b, NL);
    if nl < 0 { return if b.len > MAX_HEAD { -1 } else { 0 }; }
    var le = nl;
    if le > 0 && b[le - 1] == '\r' { le--; }
    // METHOD SP target SP HTTP/1.x
    var sp1 = -1;
    for i in le { if b[i] == ' ' { sp1 = i; break; } }
    if sp1 <= 0 { return -1; }
    var sp2 = -1;
    for i in sp1 + 1..le { if b[i] == ' ' { sp2 = i; break; } }
    if sp2 < 0 || sp2 == sp1 + 1 { return -1; }
    let ver = b[sp2 + 1..le];
    if !starts_with(ver, "HTTP/1.") { return -1; }
    let m = b[..sp1];
    r.get = m == "GET";
    r.post = m == "POST";
    r.close = ver == "HTTP/1.0";
    r.has_auth = false;
    r.p0 = sp1 + 1;
    r.p1 = sp2;
    for i in sp1 + 1..sp2 { if b[i] == '?' { r.p1 = i; break; } }
    // Headers, up to the empty line.
    var clen = 0;
    var pos = nl + 1;
    loop {
        let n = find_any(b[pos..], NL);
        if n < 0 { return if b.len > MAX_HEAD { -1 } else { 0 }; }
        var e = pos + n;
        let next = e + 1;
        if e > pos && b[e - 1] == '\r' { e--; }
        if e == pos { pos = next; break; }
        var colon = -1;
        for i in pos..e { if b[i] == ':' { colon = i; break; } }
        if colon > 0 {
            let nlen = colon - pos;
            if nlen == 13 && name_is(b, pos, colon, "authorization") {
                let s, t = trim_sp(b, colon + 1, e);
                r.has_auth = true;
                r.a0 = s;
                r.a1 = t;
            } else if nlen == 14 && name_is(b, pos, colon, "content-length") {
                let s, t = trim_sp(b, colon + 1, e);
                if s == t || t - s > 9 { return -1; }
                var v = 0;
                for i in s..t {
                    let c = b[i];
                    if c < '0' || c > '9' { return -1; }
                    v = v * 10 + (c - '0');
                }
                if v > MAX_BODY { return -1; }
                clen = v;
            } else if nlen == 17 && name_is(b, pos, colon, "transfer-encoding") {
                return -1; // chunked bodies are out of scope
            } else if nlen == 10 && name_is(b, pos, colon, "connection") {
                let s, t = trim_sp(b, colon + 1, e);
                if name_is(b, s, t, "close") { r.close = true; }
                else if name_is(b, s, t, "keep-alive") { r.close = false; }
            }
        }
        pos = next;
        if pos > MAX_HEAD { return -1; }
    }
    if b.len < pos + clen { return 0; }
    r.b0 = pos;
    r.b1 = pos + clen;
    r.used = pos + clen;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Routing

fn handle(conn: i64, b: const u8[:], r: Req&) {
    let path = b[r.p0..r.p1];
    if r.get {
        if path == "/feed" { feed(conn); return; }
        if starts_with(path, "/posts/") {
            let rest = path[7..];
            if position(rest, '/') < 0 {
                let id = positive_int(rest);
                if id <= 0 { error(conn, 400, "invalid post id"); return; }
                get_post(conn, id);
                return;
            }
        }
        if path == "/health" { health(conn); return; }
        error(conn, 404, "not found");
        return;
    }
    if r.post {
        if path == "/posts" { create_post(conn, b, r); return; }
        if path.len > 12 && starts_with(path, "/posts/") && ends_with(path, "/like") {
            let rest = path[7..path.len - 5];
            if position(rest, '/') < 0 { like_post(conn, b, r, rest); return; }
        }
    }
    error(conn, 404, "not found");
}

// A positive decimal integer with no sign, spaces or leading zeros that fits in an i64; 0 if not.
fn positive_int(s: const u8[:]) -> i64 {
    if s.len == 0 || s.len > 18 || s[0] == '0' { return 0; }
    var v = 0;
    for c in s {
        if c < '0' || c > '9' { return 0; }
        v = v * 10 + (c - '0');
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// Responses

fn status_line(status: i32) -> const u8[:] {
    return match status {
        200 => "HTTP/1.1 200 OK\r\n",
        201 => "HTTP/1.1 201 Created\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n",
        401 => "HTTP/1.1 401 Unauthorized\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n",
        503 => "HTTP/1.1 503 Service Unavailable\r\n",
        _ => "HTTP/1.1 500 Internal Server Error\r\n",
    };
}

// Sends `body` with the given status.
fn send(conn: i64, status: i32) {
    head.clear();
    head.append(status_line(status));
    head.append("Content-Type: application/json\r\nContent-Length: ");
    push_int(head, body.len);
    head.append("\r\n\r\n");
    net_reply(conn, head, body);
}

fn error(conn: i64, status: i32, msg: const u8[:]) {
    body.clear();
    body.append("{\"error\":\"");
    body.append(msg);
    body.append("\"}");
    send(conn, status);
}

fn internal(conn: i64) { error(conn, 500, "internal server error"); }

// Decimal digits of a non-negative integer (negative ones get a sign).
fn push_int(out: u8[>..]&, v: i64) {
    if v >= 0 && v < 10 { out.push(('0' + v) as! u8); return; }
    var d: u8[20] = [0; 20];
    var n = 0;
    var x = v;
    if v < 0 { out.push('-'); x = -v; }
    while x > 0 {
        d[19 - n] = ('0' + x % 10) as! u8;
        x = x / 10;
        n++;
    }
    out.append(d[20 - n..]);
}

// JSON string contents: `"`, `\` and control characters escaped, everything else (UTF-8
// included) as is. Eight bytes are tested at a time; runs with nothing to escape are copied
// in one piece.
let LO7: u64 = 0x0101010101010101;
let HI1: u64 = 0x8080808080808080;

fn needs_escape8(w: u64) -> u64 {
    let q = w ^ 0x2222222222222222;
    let s = w ^ 0x5c5c5c5c5c5c5c5c;
    let zq = (q - LO7) & ~q;
    let zs = (s - LO7) & ~s;
    let lt = (w - 0x2020202020202020) & ~w;
    return (zq | zs | lt) & HI1;
}

fn load8(s: const u8[:], i: i64) -> u64 {
    let w = s[i..i + 8]; // one check, and the eight loads below become one
    return w[0] | (w[1] as u64 << 8) | (w[2] as u64 << 16) | (w[3] as u64 << 24) |
        (w[4] as u64 << 32) | (w[5] as u64 << 40) | (w[6] as u64 << 48) | (w[7] as u64 << 56);
}

// The length of s's prefix that needs no escaping.
fn plain_prefix(s: const u8[:]) -> i64 {
    var i = 0;
    let n = s.len;
    while i + 8 <= n {
        let m = needs_escape8(load8(s, i));
        if m != 0 { return i + ctz(m) / 8; }
        i += 8;
    }
    while i < n {
        let c = s[i];
        if c < 0x20 || c == '"' || c == '\\' { return i; }
        i++;
    }
    return n;
}

let HEX = "0123456789abcdef";

fn esc(out: u8[>..]&, s: const u8[:]) {
    var i = 0;
    while i < s.len {
        let k = plain_prefix(s[i..]);
        if k > 0 { out.append(s[i..i + k]); i += k; }
        if i >= s.len { break; }
        let c = s[i];
        if c == '"' { out.append("\\\""); }
        else if c == '\\' { out.append("\\\\"); }
        else if c == '\n' { out.append("\\n"); }
        else if c == '\r' { out.append("\\r"); }
        else if c == '\t' { out.append("\\t"); }
        else if c == 8 { out.append("\\b"); }
        else if c == 12 { out.append("\\f"); }
        else {
            out.append("\\u00");
            out.push(HEX[(c >> 4) as i64]);
            out.push(HEX[(c & 15) as i64]);
        }
        i++;
    }
}

// ---------------------------------------------------------------------------------------------
// SQLite's record format: a header of serial types (varints), then the column bytes.

fn varint_at(p: const u8[:], i: i64) -> u64, i64 {
    if i < p.len && p[i] < 0x80 { return p[i], i + 1; }
    var v: u64 = 0;
    var k = i;
    for n in 9 {
        if k >= p.len { return 0, -1; }
        let b = p[k];
        k++;
        if n == 8 { return (v << 8) | b, k; }
        v = (v << 7) | (b & 0x7f);
        if b < 0x80 { return v, k; }
    }
    return v, k;
}

fn serial_len(t: u64) -> i64 {
    if t >= 12 { return ((t - 12) / 2) as i64; }
    return match t { 1 => 1, 2 => 2, 3 => 3, 4 => 4, 5 => 6, 6 => 8, 7 => 8, _ => 0 };
}

// Decodes the first n (at most 4) columns of the record at rec[s..e]: col_t the serial types,
// col_s/col_e the byte ranges in rec. false if the record is shorter or malformed.
struct Cols { t: u64[4], s: i64[4], e: i64[4] }

fn columns(rec: const u8[:], s: i64, e: i64, n: i64, c: Cols&) -> bool {
    let hsize, i0 = varint_at(rec[..e], s);
    if i0 < 0 { return false; }
    let hend = s + hsize as i64;
    if hsize as i64 > e - s { return false; }
    var i = i0;
    var at = hend;
    for k in n {
        if i >= hend { return false; }
        let t, ni = varint_at(rec[..hend], i);
        if ni < 0 { return false; }
        i = ni;
        let l = serial_len(t);
        if at + l > e { return false; }
        c.t[k] = t;
        c.s[k] = at;
        c.e[k] = at + l;
        at += l;
    }
    return true;
}

fn is_text(t: u64) -> bool { return t >= 13 && (t & 1) == 1; }

// The integer in column k (serial types 1-6, 8, 9). false for any other type.
fn int_col(rec: const u8[:], c: Cols&, k: i64) -> i64, bool {
    let t = c.t[k];
    if t == 8 { return 0, true; }
    if t == 9 { return 1, true; }
    if t < 1 || t > 6 { return 0, false; }
    let s = c.s[k];
    let e = c.e[k];
    var v = rec[s] as i8 as i64;
    for i in s + 1..e { v = (v << 8) | rec[i]; }
    return v, true;
}

// ---------------------------------------------------------------------------------------------
// Reads

// Likes of one post: a range walk of likes_post_id_idx over the one key.
fn count_likes(id: i64) -> i64 {
    like_counts[0] = 0;
    let rc = bt_count_likes(id, id, like_counts[0..1]);
    if rc == 0 { return like_counts[0]; }
    if rc < 0 { return -1; }
    return count_likes_slow(id);
}

// Likes of one post, decoding each index entry here (for cells bt_count_likes does not expect).
fn count_likes_slow(id: i64) -> i64 {
    var r = bt_seek_ge(C_LIKES, id);
    var n = 0;
    var c = Cols { .. };
    while r == 1 {
        let s = text.len;
        let len = bt_payload(C_LIKES, text);
        if len < 0 { return -1; }
        if !columns(text, s, s + len, 1, c) { return -1; }
        let k, ok = int_col(text, c, 0);
        if !ok { return -1; }
        if k != id { break; }
        n++;
        r = bt_next(C_LIKES);
    }
    if r < 0 { return -1; }
    return n;
}

// Appends the username of user `uid` to text: 1 found, 0 no such user, -1 error.
fn author(uid: i64, a0: i64&, a1: i64&) -> i32 {
    let f = bt_seek(C_USERS, uid);
    if f != 1 { return f; }
    let s = text.len;
    let len = bt_payload(C_USERS, text);
    if len < 0 { return -1; }
    var c = Cols { .. };
    if !columns(text, s, s + len, 2, c) || !is_text(c.t[1]) { return -1; }
    a0 = c.s[1];
    a1 = c.e[1];
    return 1;
}

// GET /posts/:id
fn get_post(conn: i64, id: i64) {
    if db_begin() != 0 { internal(conn); return; }
    text.clear();
    let f = bt_seek(C_POSTS, id);
    if f < 0 { internal(conn); return; }
    if f == 0 { error(conn, 404, "post not found"); return; }
    let s = text.len;
    let len = bt_payload(C_POSTS, text);
    if len < 0 { internal(conn); return; }
    var c = Cols { .. };
    if !columns(text, s, s + len, 4, c) || !is_text(c.t[2]) || !is_text(c.t[3]) { internal(conn); return; }
    let uid, ok = int_col(text, c, 1);
    if !ok { internal(conn); return; }
    var a0 = 0;
    var a1 = 0;
    let fa = author(uid, a0, a1);
    if fa < 0 { internal(conn); return; }
    if fa == 0 { error(conn, 404, "post not found"); return; } // the JOIN finds no row
    let likes = count_likes(id);
    if likes < 0 { internal(conn); return; }
    body.clear();
    body.append("{\"post\":{\"id\":");
    push_int(body, id);
    body.append(",\"body\":\"");
    esc(body, text[c.s[2]..c.e[2]]);
    body.append("\",\"created_at\":\"");
    esc(body, text[c.s[3]..c.e[3]]);
    body.append("\",\"author\":\"");
    esc(body, text[a0..a1]);
    body.append("\",\"like_count\":");
    push_int(body, likes);
    body.append("}}");
    send(conn, 200);
}

// GET /feed: the 20 newest by (created_at DESC, id DESC), read off that index.
fn feed(conn: i64) {
    if db_begin() != 0 { internal(conn); return; }
    let st = feed_rows();
    if st < 0 { internal(conn); return; }
    if st == 0 { feed_sql(conn); return; } // data the B-tree plan does not expect: SQL answers
    body.clear();
    body.append("{\"posts\":[");
    for k in nrows {
        let row = rows[k];
        if k > 0 { body.push(','); }
        body.append("{\"id\":");
        push_int(body, row.id);
        body.append(",\"body\":\"");
        esc(body, text[row.b0..row.b1]);
        body.append("\",\"created_at\":\"");
        esc(body, text[row.c0..row.c1]);
        body.append("\",\"author\":\"");
        esc(body, text[row.a0..row.a1]);
        body.append("\",\"like_count\":");
        push_int(body, row.likes);
        body.push('}');
    }
    body.append("]}");
    send(conn, 200);
}

// Fills rows: 1 ok, 0 the reference SQL should answer instead, -1 error. The plan assumes
// nothing about the data: not dense ids, not timestamps in id order, no remembered state.
fn feed_rows() -> i32 {
    text.clear();
    nrows = 0;
    var c = Cols { .. };
    // 1. The index (created_at DESC, id DESC): its records hold created_at and id.
    var r = bt_first(C_FEED);
    while r == 1 && nrows < FEED_LEN {
        let s = text.len;
        let len = bt_payload(C_FEED, text);
        if len < 0 { return -1; }
        if !columns(text, s, s + len, 2, c) || !is_text(c.t[0]) { return -1; }
        let id, ok = int_col(text, c, 1);
        if !ok { return -1; }
        rows[nrows] = Row { id: id, uid: 0, b0: 0, b1: 0, c0: c.s[0], c1: c.e[0], a0: 0, a1: 0, likes: 0 };
        nrows++;
        r = bt_next(C_FEED);
    }
    if r < 0 { return -1; }
    let n = nrows;
    if n == 0 { return 1; }
    // 2. The posts, in ascending id order: consecutive ids are the next cell of the same leaf.
    // Newest-first is normally descending id, so ascending is the reverse.
    for k in n { order[k] = n - 1 - k; }
    var sorted = true;
    for k in 1..n { if rows[order[k - 1]].id >= rows[order[k]].id { sorted = false; } }
    if !sorted {
        for k in 1..n {
            let v = order[k];
            var j = k;
            while j > 0 && rows[order[j - 1]].id > rows[v].id { order[j] = order[j - 1]; j--; }
            order[j] = v;
        }
    }
    for k in n {
        let i = order[k];
        if bt_seek(C_POSTS, rows[i].id) != 1 { return 0; } // index and table disagree
        let s = text.len;
        let len = bt_payload(C_POSTS, text);
        if len < 0 { return -1; }
        if !columns(text, s, s + len, 3, c) || !is_text(c.t[2]) { return -1; }
        let uid, ok = int_col(text, c, 1);
        if !ok { return -1; }
        rows[i].uid = uid;
        rows[i].b0 = c.s[2];
        rows[i].b1 = c.e[2];
    }
    // 3. Authors.
    for k in n {
        var a0 = 0;
        var a1 = 0;
        let f = author(rows[k].uid, a0, a1);
        if f == 0 { return 0; } // a post without a user: the JOIN drops it
        if f < 0 { return -1; }
        rows[k].a0 = a0;
        rows[k].a1 = a1;
    }
    // 4. Like counts: one walk over likes_post_id_idx when the ids are close together.
    let lo = rows[order[0]].id;
    let hi = rows[order[n - 1]].id;
    if hi - lo <= RANGE_SPAN {
        let span = hi - lo + 1;
        for k in span { like_counts[k] = 0; }
        let rc = bt_count_likes(lo, hi, like_counts[0..span]);
        if rc < 0 { return -1; }
        if rc == 0 {
            for k in n { rows[k].likes = like_counts[rows[k].id - lo]; }
            return 1;
        }
        for k in n {
            let v = count_likes_slow(rows[k].id);
            if v < 0 { return -1; }
            rows[k].likes = v;
        }
        return 1;
    }
    for k in n {
        let v = count_likes(rows[k].id);
        if v < 0 { return -1; }
        rows[k].likes = v;
    }
    return 1;
}

// The reference query, for data the B-tree plan does not expect.
fn feed_sql(conn: i64) {
    body.clear();
    body.append("{\"posts\":[");
    var row = SqlRow { id: 0, likes: 0, blen: 0, clen: 0, alen: 0 };
    var first = true;
    loop {
        text.clear();
        let rc = db_feed_sql_step(row, text);
        if rc < 0 { internal(conn); return; }
        if rc == 0 { break; }
        if !first { body.push(','); }
        first = false;
        let b1 = row.blen;
        let c1 = b1 + row.clen;
        let a1 = c1 + row.alen;
        body.append("{\"id\":");
        push_int(body, row.id);
        body.append(",\"body\":\"");
        esc(body, text[0..b1]);
        body.append("\",\"created_at\":\"");
        esc(body, text[b1..c1]);
        body.append("\",\"author\":\"");
        esc(body, text[c1..a1]);
        body.append("\",\"like_count\":");
        push_int(body, row.likes);
        body.push('}');
    }
    body.append("]}");
    send(conn, 200);
}

// GET /health: SELECT 1 inside the pass's transaction.
fn health(conn: i64) {
    body.clear();
    if db_begin() != 0 {
        body.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"cannot begin transaction\"}");
        send(conn, 503);
        return;
    }
    scratch.clear();
    if db_health(scratch) != 0 {
        body.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        esc(body, scratch);
        body.append("\"}");
        send(conn, 503);
        return;
    }
    body.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    push_int(body, (now_mono_ms() - started_ms) / 1000);
    body.push('}');
    send(conn, 200);
}

// ---------------------------------------------------------------------------------------------
// Writes

// POST /posts
fn create_post(conn: i64, b: const u8[:], r: Req&) {
    var username: u8[>..] = [];
    let uid = authenticate(conn, b, r, username);
    if uid <= 0 { return; }
    // The request body: {"body": "..."}; the last duplicate key wins, as in JSON.parse.
    let src = b[r.b0..r.b1];
    var p = 0;
    var vs = -1;
    var ve = -1;
    var vesc = false;
    var is_str = false;
    let st = object_members(src, p, scratch) { key =>
        if key == "body" {
            if p < src.len && src[p] == '"' {
                let s, e, escd, ok = read_string(src, p);
                if !ok { return -1 from object_members; }
                vs = s; ve = e; vesc = escd; is_str = true;
            } else {
                if !skip_value(src, p) { return -1 from object_members; }
                is_str = false;
            }
        } else {
            if !skip_value(src, p) { return -1 from object_members; }
        }
    };
    if st < 0 { error(conn, 400, "malformed JSON body"); return; }
    if st == 0 || !is_str { error(conn, 400, "body is required"); return; }
    scratch.clear();
    if vesc { unescape(src[vs..ve], scratch); } else { scratch.append(src[vs..ve]); }
    let ts, te, chars = trim_count(scratch);
    if chars < 0 { error(conn, 400, "malformed JSON body"); return; }
    if chars == 0 { error(conn, 400, "body is required"); return; }
    if chars > MAX_CHARS { error(conn, 400, "body must be at most 500 characters"); return; }
    let post_body = scratch[ts..te];
    if db_begin() != 0 { internal(conn); return; }
    var id = 0;
    text.clear();
    if db_insert_post(uid, post_body, id, text) != 0 { internal(conn); return; }
    body.clear();
    body.append("{\"post\":{\"id\":");
    push_int(body, id);
    body.append(",\"body\":\"");
    esc(body, post_body);
    body.append("\",\"created_at\":\"");
    esc(body, text);
    body.append("\",\"author\":\"");
    esc(body, username);
    body.append("\",\"like_count\":0}}");
    send(conn, 201);
}

// POST /posts/:id/like (auth first, then the id)
fn like_post(conn: i64, b: const u8[:], r: Req&, id_s: const u8[:]) {
    var username: u8[>..] = [];
    let uid = authenticate(conn, b, r, username);
    if uid <= 0 { return; }
    let id = positive_int(id_s);
    if id <= 0 { error(conn, 400, "invalid post id"); return; }
    if db_begin() != 0 { internal(conn); return; }
    let f = bt_seek(C_POSTS, id);
    if f < 0 { internal(conn); return; }
    if f == 0 { error(conn, 404, "post not found"); return; }
    let rc = db_insert_like(uid, id);
    if rc < 0 { internal(conn); return; }
    body.clear();
    if rc == 1 { body.append("{\"liked\":true,\"already_liked\":false,\"post_id\":"); }
    else { body.append("{\"liked\":true,\"already_liked\":true,\"post_id\":"); }
    push_int(body, id);
    body.push('}');
    send(conn, if rc == 1 { 201 } else { 200 });
}

// JavaScript's String.prototype.trim whitespace: Unicode White_Space without NEL, plus BOM.
fn js_space(cp: i64) -> bool {
    if cp < 0x80 { return (cp >= 9 && cp <= 13) || cp == 32; }
    return cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
        cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

// One code point of well-formed UTF-8 at s[i]: (code point, bytes), or (0, 0) if malformed.
fn utf8_at(s: const u8[:], i: i64) -> i64, i64 {
    let b = s[i];
    if b < 0x80 { return b, 1; }
    var n = 0;
    var cp = 0;
    if b >= 0xc2 && b <= 0xdf { n = 2; cp = (b & 0x1f); }
    else if b >= 0xe0 && b <= 0xef { n = 3; cp = (b & 0x0f); }
    else if b >= 0xf0 && b <= 0xf4 { n = 4; cp = (b & 0x07); }
    else { return 0, 0; }
    if i + n > s.len { return 0, 0; }
    for k in 1..n {
        let x = s[i + k];
        if (x & 0xc0) != 0x80 { return 0, 0; }
        cp = (cp << 6) | (x & 0x3f);
    }
    if (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff {
        return 0, 0;
    }
    return cp, n;
}

// The body trimmed (byte range) and its length in characters (code points, the unit of the
// schema's CHECK); -1 characters if it is not well-formed UTF-8.
fn trim_count(s: const u8[:]) -> i64, i64, i64 {
    var ascii = true;
    for c in s { if c >= 0x80 { ascii = false; break; } }
    if ascii {
        var i = 0;
        var j = s.len;
        while i < j && js_space(s[i]) { i++; }
        while j > i && js_space(s[j - 1]) { j--; }
        return i, j, j - i;
    }
    var start = -1;
    var end = 0;
    var count = 0;
    var kept = 0;
    var i = 0;
    while i < s.len {
        let cp, n = utf8_at(s, i);
        if n == 0 { return 0, 0, -1; }
        if !js_space(cp) {
            if start < 0 { start = i; }
            count++;
            kept = count;
            end = i + n;
        } else if start >= 0 {
            count++;
        }
        i += n;
    }
    if start < 0 { return 0, 0, 0; }
    return start, end, kept;
}

// ---------------------------------------------------------------------------------------------
// JSON input: a validating reader over a whole document.

fn ws(s: const u8[:], p: i64&) {
    while p < s.len {
        let c = s[p];
        if c != ' ' && c != '\t' && c != '\n' && c != '\r' { return; }
        p++;
    }
}

fn hexval(c: u8) -> i64 {
    if c >= '0' && c <= '9' { return c - '0'; }
    if c >= 'a' && c <= 'f' { return (c - 'a') as i64 + 10; }
    if c >= 'A' && c <= 'F' { return (c - 'A') as i64 + 10; }
    return -1;
}

fn hex4(s: const u8[:], i: i64) -> i64 {
    if i + 4 > s.len { return -1; }
    var v = 0;
    for k in 4 {
        let h = hexval(s[i + k]);
        if h < 0 { return -1; }
        v = v * 16 + h;
    }
    return v;
}

fn valid_utf8(s: const u8[:]) -> bool {
    var i = 0;
    while i < s.len {
        if s[i] < 0x80 { i++; continue; }
        let cp, n = utf8_at(s, i);
        if n == 0 { return false; }
        i += n;
    }
    return true;
}

// The string at s[p] (which is '"'): its raw contents s[start..end], whether it has escapes,
// and whether it is well formed (escapes, no raw control characters, UTF-8). p moves past it.
fn read_string(s: const u8[:], p: i64&) -> i64, i64, bool, bool {
    let start = p + 1;
    var i = start;
    var escd = false;
    var high = false;
    loop {
        i += plain_prefix(s[i..]);
        if i >= s.len { return 0, 0, false, false; }
        let c = s[i];
        if c == '"' { break; }
        if c != '\\' { return 0, 0, false, false; } // a raw control character
        escd = true;
        if i + 1 >= s.len { return 0, 0, false, false; }
        let e = s[i + 1];
        if e == 'u' {
            if hex4(s, i + 2) < 0 { return 0, 0, false, false; }
            i += 6;
        } else if e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't' {
            i += 2;
        } else {
            return 0, 0, false, false;
        }
    }
    let raw = s[start..i];
    for c in raw { if c >= 0x80 { high = true; break; } }
    if high && !valid_utf8(raw) { return 0, 0, false, false; }
    p = i + 1;
    return start, i, escd, true;
}

fn put_utf8(out: u8[>..]&, cp: i64) {
    if cp < 0x80 { out.push(cp as! u8); }
    else if cp < 0x800 { out.push((0xc0 | (cp >> 6)) as! u8); out.push((0x80 | (cp & 0x3f)) as! u8); }
    else if cp < 0x10000 {
        out.push((0xe0 | (cp >> 12)) as! u8);
        out.push((0x80 | ((cp >> 6) & 0x3f)) as! u8);
        out.push((0x80 | (cp & 0x3f)) as! u8);
    } else {
        out.push((0xf0 | (cp >> 18)) as! u8);
        out.push((0x80 | ((cp >> 12) & 0x3f)) as! u8);
        out.push((0x80 | ((cp >> 6) & 0x3f)) as! u8);
        out.push((0x80 | (cp & 0x3f)) as! u8);
    }
}

// Decodes the escapes of a well-formed string's raw contents (read_string checked them).
// A surrogate pair becomes one code point; a lone surrogate becomes U+FFFD.
fn unescape(raw: const u8[:], out: u8[>..]&) {
    var i = 0;
    while i < raw.len {
        let c = raw[i];
        if c != '\\' { out.push(c); i++; continue; }
        let e = raw[i + 1];
        if e == 'u' {
            var cp = hex4(raw, i + 2);
            i += 6;
            if cp >= 0xd800 && cp <= 0xdbff && i + 6 <= raw.len && raw[i] == '\\' && raw[i + 1] == 'u' {
                let lo = hex4(raw, i + 2);
                if lo >= 0xdc00 && lo <= 0xdfff {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                }
            }
            if cp >= 0xd800 && cp <= 0xdfff { cp = 0xfffd; }
            put_utf8(out, cp);
            continue;
        }
        let d: u8 = match e { 'b' => 8, 'f' => 12, 'n' => '\n', 'r' => '\r', 't' => '\t', _ => e };
        out.push(d);
        i += 2;
    }
}

fn lit(s: const u8[:], p: i64&, word: const u8[:]) -> bool {
    if p + word.len > s.len || s[p..p + word.len] != word { return false; }
    p += word.len;
    return true;
}

// A JSON number at s[p]: its value, and whether it is well formed. p moves past it.
fn read_number(s: const u8[:], p: i64&) -> f64, bool {
    let start = p;
    var i = p;
    if i < s.len && s[i] == '-' { i++; }
    if i >= s.len { return 0.0, false; }
    if s[i] == '0' { i++; }
    else if s[i] >= '1' && s[i] <= '9' { while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; } }
    else { return 0.0, false; }
    if i < s.len && s[i] == '.' {
        i++;
        let d = i;
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
        if i == d { return 0.0, false; }
    }
    if i < s.len && (s[i] == 'e' || s[i] == 'E') {
        i++;
        if i < s.len && (s[i] == '+' || s[i] == '-') { i++; }
        let d = i;
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
        if i == d { return 0.0, false; }
    }
    let v, ok = parse_flt(s[start..i]);
    if !ok { return 0.0, false; }
    p = i;
    return v, true;
}

// Skips one value at s[p] (after whitespace), validating it; nested containers are walked with
// an explicit stack, so no input can recurse.
fn skip_value(s: const u8[:], p: i64&) -> bool {
    var stack: bool[512] = [false; 512]; // true: an object
    var depth = 0;
    loop {
        // A value is expected here.
        ws(s, p);
        if p >= s.len { return false; }
        let c = s[p];
        var opened = false;
        if c == '{' || c == '[' {
            p++;
            ws(s, p);
            if p >= s.len { return false; }
            let close: u8 = if c == '{' { '}' } else { ']' };
            if s[p] == close {
                p++;
            } else {
                if depth >= 512 { return false; }
                stack[depth] = c == '{';
                depth++;
                opened = true;
                if c == '{' {
                    if s[p] != '"' { return false; }
                    let a, b, e, ok = read_string(s, p);
                    if !ok { return false; }
                    ws(s, p);
                    if p >= s.len || s[p] != ':' { return false; }
                    p++;
                }
            }
        } else if c == '"' {
            let a, b, e, ok = read_string(s, p);
            if !ok { return false; }
        } else if c == 't' {
            if !lit(s, p, "true") { return false; }
        } else if c == 'f' {
            if !lit(s, p, "false") { return false; }
        } else if c == 'n' {
            if !lit(s, p, "null") { return false; }
        } else {
            let v, ok = read_number(s, p);
            if !ok { return false; }
        }
        if opened { continue; }
        // After a value: close containers, or move to the next element.
        var more = false;
        while depth > 0 && !more {
            ws(s, p);
            if p >= s.len { return false; }
            let d = s[p];
            let obj = stack[depth - 1];
            if d == ',' {
                p++;
                if obj {
                    ws(s, p);
                    if p >= s.len || s[p] != '"' { return false; }
                    let a, b, e, ok = read_string(s, p);
                    if !ok { return false; }
                    ws(s, p);
                    if p >= s.len || s[p] != ':' { return false; }
                    p++;
                }
                more = true;
            } else if (obj && d == '}') || (!obj && d == ']') {
                p++;
                depth--;
            } else {
                return false;
            }
        }
        if !more { return true; }
    }
}

// Walks a whole JSON document: for each member of a top-level object F(key) is called with p at
// the member's value, which F must consume. 1 an object, 0 valid JSON that is not an object, -1
// invalid. Escaped member names are decoded (into kbuf) before F sees them.
fn object_members<F>(s: const u8[:], p: i64&, kbuf: u8[>..]&) -> i32 {
    p = 0;
    ws(s, p);
    if p >= s.len { return -1; }
    if s[p] != '{' {
        if !skip_value(s, p) { return -1; }
        ws(s, p);
        return if p == s.len { 0 } else { -1 };
    }
    p++;
    ws(s, p);
    if p < s.len && s[p] == '}' {
        p++;
    } else {
        loop {
            ws(s, p);
            if p >= s.len || s[p] != '"' { return -1; }
            let ks, ke, kesc, ok = read_string(s, p);
            if !ok { return -1; }
            ws(s, p);
            if p >= s.len || s[p] != ':' { return -1; }
            p++;
            ws(s, p);
            if kesc {
                kbuf.clear();
                unescape(s[ks..ke], kbuf);
                F(kbuf[..]);
            } else {
                F(s[ks..ke]);
            }
            ws(s, p);
            if p >= s.len { return -1; }
            if s[p] == ',' { p++; continue; }
            if s[p] == '}' { p++; break; }
            return -1;
        }
    }
    ws(s, p);
    return if p == s.len { 1 } else { -1 };
}

// ---------------------------------------------------------------------------------------------
// HS256: SHA-256, HMAC, base64url and the claims. Every token's signature is checked on every
// request; only the key's padded blocks are hashed once at startup.

let K256: u32[64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

let H256: u32[8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

fn rotr(x: u32, n: u32) -> u32 { return (x >> n) | (x << (32 - n)); }

// One 64-byte block at blk[at..at + 64].
fn compress(h: u32[8]&, blk: const u8[:], at: i64) {
    var w: u32[64] = [0; 64];
    for i in 16 {
        let j = at + i * 4;
        w[i] = (blk[j] as u32 << 24) | (blk[j + 1] as u32 << 16) | (blk[j + 2] as u32 << 8) | blk[j + 3];
    }
    for i in 16..64 {
        let a = w[i - 15];
        let b = w[i - 2];
        let s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3);
        let s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    var a = h[0]; var b = h[1]; var c = h[2]; var d = h[3];
    var e = h[4]; var f = h[5]; var g = h[6]; var k = h[7];
    for i in 64 {
        let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        let ch = (e & f) ^ (~e & g);
        let t1 = k + s1 + ch + K256[i] + w[i];
        let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let t2 = s0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

// Finishes a hash whose state h has already absorbed `before` bytes (whole blocks), over msg.
fn sha_finish(h: u32[8]&, msg: const u8[:], before: i64, out: u8[32]&) {
    var i = 0;
    while i + 64 <= msg.len { compress(h, msg, i); i += 64; }
    var tail: u8[128] = [0; 128];
    let rest = msg.len - i;
    for k in rest { tail[k] = msg[i + k]; }
    tail[rest] = 0x80;
    let blocks = if rest + 9 <= 64 { 1 } else { 2 };
    let bits = ((before + msg.len) * 8) as u64;
    let end = blocks * 64;
    for k in 8 { tail[end - 1 - k] = (bits >> (k * 8) as u64) as! u8; }
    compress(h, tail, 0);
    if blocks == 2 { compress(h, tail, 64); }
    for k in 8 {
        out[k * 4] = (h[k] >> 24) as! u8;
        out[k * 4 + 1] = (h[k] >> 16) as! u8;
        out[k * 4 + 2] = (h[k] >> 8) as! u8;
        out[k * 4 + 3] = h[k] as! u8;
    }
}

fn hmac_key(secret: const u8[:]) {
    var key: u8[64] = [0; 64];
    if secret.len > 64 {
        var h = H256;
        var d: u8[32] = [0; 32];
        sha_finish(h, secret, 0, d);
        for k in 32 { key[k] = d[k]; }
    } else {
        for k in secret.len { key[k] = secret[k]; }
    }
    var ip: u8[64] = [0; 64];
    var op: u8[64] = [0; 64];
    for k in 64 { ip[k] = key[k] ^ 0x36; op[k] = key[k] ^ 0x5c; }
    secret_inner = H256;
    compress(secret_inner, ip, 0);
    secret_outer = H256;
    compress(secret_outer, op, 0);
}

// HMAC-SHA256(key, msg) from the precomputed pad states.
fn hmac(msg: const u8[:], out: u8[32]&) {
    var h = secret_inner;
    var inner: u8[32] = [0; 32];
    sha_finish(h, msg, 64, inner);
    var g = secret_outer;
    sha_finish(g, inner, 64, out);
}

fn b64val(c: u8) -> i64 {
    if c >= 'A' && c <= 'Z' { return c - 'A'; }
    if c >= 'a' && c <= 'z' { return (c - 'a') as i64 + 26; }
    if c >= '0' && c <= '9' { return (c - '0') as i64 + 52; }
    if c == '-' { return 62; }
    if c == '_' { return 63; }
    return -1;
}

// Unpadded base64url, appended to out. Only the canonical spelling is accepted: the unused low
// bits of the last character must be zero, so exactly one spelling of a signature verifies.
fn b64url(s: const u8[:], out: u8[>..]&) -> bool {
    if s.len % 4 == 1 { return false; }
    var i = 0;
    while i + 4 <= s.len {
        let a = b64val(s[i]); let b = b64val(s[i + 1]); let c = b64val(s[i + 2]); let d = b64val(s[i + 3]);
        if (a | b | c | d) < 0 { return false; }
        let v = (a << 18) | (b << 12) | (c << 6) | d;
        out.push((v >> 16) as! u8);
        out.push((v >> 8) as! u8);
        out.push(v as! u8);
        i += 4;
    }
    let r = s.len - i;
    if r == 0 { return true; }
    var v = 0;
    for k in r {
        let x = b64val(s[i + k]);
        if x < 0 { return false; }
        v = v | (x << (18 - 6 * k));
    }
    let unused = if r == 2 { v & 0xffff } else { v & 0xff };
    if unused != 0 { return false; }
    out.push((v >> 16) as! u8);
    if r == 3 { out.push((v >> 8) as! u8); }
    return true;
}

// Verifies the request's bearer token. Returns the user id (> 0, with the username appended),
// or 0 after answering 401.
fn authenticate(conn: i64, b: const u8[:], r: Req&, username: u8[>..]&) -> i64 {
    if !r.has_auth || !starts_with(b[r.a0..r.a1], "Bearer ") {
        error(conn, 401, "missing bearer token");
        return 0;
    }
    let tok = b[r.a0 + 7..r.a1];
    // header.payload.signature
    var d1 = -1;
    var d2 = -1;
    var dots = 0;
    for c, i in tok {
        if c == '.' {
            dots++;
            if dots == 1 { d1 = i; } else if dots == 2 { d2 = i; }
        }
    }
    if dots != 2 { error(conn, 401, "invalid or expired token"); return 0; }
    jwt_head.clear();
    if !b64url(tok[d2 + 1..], jwt_head) || jwt_head.len != 32 {
        error(conn, 401, "invalid or expired token");
        return 0;
    }
    var mac: u8[32] = [0; 32];
    hmac(tok[..d2], mac);
    var diff: u8 = 0;
    for k in 32 { diff |= mac[k] ^ jwt_head[k]; }
    if diff != 0 { error(conn, 401, "invalid or expired token"); return 0; }
    // The header must say HS256.
    jwt_head.clear();
    if !b64url(tok[..d1], jwt_head) { error(conn, 401, "invalid or expired token"); return 0; }
    var alg_ok = false;
    var p = 0;
    let hs = jwt_head[..];
    let hst = object_members(hs, p, scratch) { key =>
        if key == "alg" {
            if p < hs.len && hs[p] == '"' {
                let s, e, escd, ok = read_string(hs, p);
                if !ok { return -1 from object_members; }
                if escd {
                    scratch2.clear();
                    unescape(hs[s..e], scratch2);
                    alg_ok = scratch2 == "HS256";
                } else {
                    alg_ok = hs[s..e] == "HS256";
                }
            } else {
                if !skip_value(hs, p) { return -1 from object_members; }
                alg_ok = false;
            }
        } else {
            if !skip_value(hs, p) { return -1 from object_members; }
        }
    };
    if hst != 1 || !alg_ok { error(conn, 401, "invalid or expired token"); return 0; }
    // The payload: exp (required, in the future), then sub and username.
    jwt_body.clear();
    if !b64url(tok[d1 + 1..d2], jwt_body) { error(conn, 401, "invalid or expired token"); return 0; }
    let ps = jwt_body[..];
    var exp = 0.0;
    var has_exp = false;
    var sub_s = -1; var sub_e = -1; var sub_esc = false;
    var nm_s = -1; var nm_e = -1; var nm_esc = false;
    p = 0;
    let pst = object_members(ps, p, scratch) { key =>
        if key == "exp" {
            let c = if p < ps.len { ps[p] } else { 0 };
            if c == '-' || (c >= '0' && c <= '9') {
                let v, ok = read_number(ps, p);
                if !ok { return -1 from object_members; }
                exp = v;
                has_exp = true;
            } else {
                if !skip_value(ps, p) { return -1 from object_members; }
                has_exp = false;
            }
        } else if key == "sub" {
            if p < ps.len && ps[p] == '"' {
                let s, e, escd, ok = read_string(ps, p);
                if !ok { return -1 from object_members; }
                sub_s = s; sub_e = e; sub_esc = escd;
            } else {
                if !skip_value(ps, p) { return -1 from object_members; }
                sub_s = -1;
            }
        } else if key == "username" {
            if p < ps.len && ps[p] == '"' {
                let s, e, escd, ok = read_string(ps, p);
                if !ok { return -1 from object_members; }
                nm_s = s; nm_e = e; nm_esc = escd;
            } else {
                if !skip_value(ps, p) { return -1 from object_members; }
                nm_s = -1;
            }
        } else {
            if !skip_value(ps, p) { return -1 from object_members; }
        }
    };
    if pst != 1 { error(conn, 401, "invalid or expired token"); return 0; }
    if !has_exp || !(now_unix() < exp) { error(conn, 401, "invalid or expired token"); return 0; }
    if sub_s < 0 || nm_s < 0 { error(conn, 401, "invalid token payload"); return 0; }
    var uid = 0;
    if sub_esc {
        scratch2.clear();
        unescape(ps[sub_s..sub_e], scratch2);
        uid = positive_int(scratch2);
    } else {
        uid = positive_int(ps[sub_s..sub_e]);
    }
    if uid <= 0 { error(conn, 401, "invalid token payload"); return 0; }
    if nm_esc { unescape(ps[nm_s..nm_e], username); } else { username.append(ps[nm_s..nm_e]); }
    return uid;
}
