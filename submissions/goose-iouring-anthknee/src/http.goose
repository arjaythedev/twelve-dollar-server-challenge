import std;
import src.model;
namespace http;

let NEWLINE = byte_set("\n");

fn trim(bytes: const u8[:], start: i64, end: i64) -> i64, i64 {
    var a = start;
    var b = end;
    while a < b && (bytes[a] == ' ' || bytes[a] == '\t') { a++; }
    while b > a && (bytes[b - 1] == ' ' || bytes[b - 1] == '\t') { b--; }
    return a, b;
}

fn equal_ascii(bytes: const u8[:], text: const u8[:]) -> bool {
    if bytes.len != text.len { return false; }
    for c, i in bytes {
        let lower = if c >= 'A' && c <= 'Z' { c + 32 } else { c };
        if lower != text[i] { return false; }
    }
    return true;
}

// No I/O. Complete ranges refer only to this input. Incomplete input is not an error.
fn parse(bytes: const u8[:]) -> model::Parse {
    let newline = find_any(bytes, NEWLINE);
    if newline < 0 {
        return if bytes.len > model::MAX_HEAD { model::Parse.Invalid } else { model::Parse.NeedMore };
    }
    var end = newline;
    if end > 0 && bytes[end - 1] == '\r' { end--; }
    let first = position(bytes[..end], ' ');
    if first <= 0 { return model::Parse.Invalid; }
    let second_rel = position(bytes[first + 1..end], ' ');
    if second_rel <= 0 { return model::Parse.Invalid; }
    let second = first + 1 + second_rel;
    let version = bytes[second + 1..end];
    if version != "HTTP/1.1" && version != "HTTP/1.0" { return model::Parse.Invalid; }
    let verb = bytes[..first];
    let method = if verb == "GET" { model::Method.Get }
        else if verb == "POST" { model::Method.Post } else { model::Method.Other };
    var req = model::Request {
        method: method, close: version == "HTTP/1.0",
        path: model::ByteRange { start: first + 1, end: second },
        auth: model::ByteRange { start: 0, end: 0 }, has_auth: false,
        body: model::ByteRange { start: 0, end: 0 }, used: 0,
    };
    let query = position(bytes[req.path.start..req.path.end], '?');
    if query >= 0 { req.path.end = req.path.start + query; }
    var at = newline + 1;
    var length = 0;
    var has_length = false;
    // Each iteration consumes at least one header byte, bounded by MAX_HEAD.
    while at <= model::MAX_HEAD {
        let relative = find_any(bytes[at..], NEWLINE);
        if relative < 0 {
            return if bytes.len > model::MAX_HEAD { model::Parse.Invalid } else { model::Parse.NeedMore };
        }
        var line_end = at + relative;
        let next = line_end + 1;
        if next > model::MAX_HEAD { return model::Parse.Invalid; }
        if line_end > at && bytes[line_end - 1] == '\r' { line_end--; }
        if line_end == at {
            if bytes.len - next < length { return model::Parse.NeedMore; }
            req.body = model::ByteRange { start: next, end: next + length };
            req.used = next + length;
            return model::Parse.Complete { request: req };
        }
        let colon_rel = position(bytes[at..line_end], ':');
        if colon_rel <= 0 { return model::Parse.Invalid; }
        let colon = at + colon_rel;
        let name = bytes[at..colon];
        let a, b = trim(bytes, colon + 1, line_end);
        let value = bytes[a..b];
        if equal_ascii(name, "authorization") {
            req.auth = model::ByteRange { start: a, end: b };
            req.has_auth = true;
        } else if equal_ascii(name, "content-length") {
            if value.len == 0 || value.len > 9 { return model::Parse.Invalid; }
            var n = 0;
            for c in value {
                if c < '0' || c > '9' { return model::Parse.Invalid; }
                n = n * 10 + c - '0';
            }
            if n > model::MAX_BODY || (has_length && n != length) { return model::Parse.Invalid; }
            length = n;
            has_length = true;
        } else if equal_ascii(name, "transfer-encoding") {
            return model::Parse.Invalid;
        } else if equal_ascii(name, "connection") {
            if equal_ascii(value, "close") { req.close = true; }
            else if equal_ascii(value, "keep-alive") { req.close = false; }
        }
        at = next;
    }
    return model::Parse.Invalid;
}

// Like IDs are deliberately not validated here: authentication has precedence.
fn route(bytes: const u8[:], req: model::Request&) -> model::Route {
    let path = bytes[req.path.start..req.path.end];
    match req.method {
        Get => {
            if path == "/health" { return model::Route.Health; }
            if path == "/feed" { return model::Route.Feed; }
            if starts_with(path, "/posts/") && position(path[7..], '/') < 0 {
                let id = model::positive_int(path[7..]);
                return if id > 0 { model::Route.Read { id: id } } else { model::Route.InvalidId };
            }
        },
        Post => {
            if path == "/posts" { return model::Route.Create; }
            if path.len >= 12 && starts_with(path, "/posts/") && ends_with(path, "/like") {
                let raw = path[7..path.len - 5];
                if position(raw, '/') < 0 {
                    return model::Route.Like { raw_id: model::ByteRange {
                        start: req.path.start + 7, end: req.path.end - 5,
                    } };
                }
            }
        },
        Other => {},
    }
    return model::Route.Missing;
}
