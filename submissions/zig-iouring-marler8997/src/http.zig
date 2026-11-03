//! HTTP/1.1 request parsing on a `std.Io.Reader`. Only the headers the API
//! needs are looked at.

/// The largest request (head and body) a connection's read buffer holds.
pub const max_request = 20 * 1024;

pub const Method = enum { get, post, other };

pub const Request = struct {
    method: Method,
    /// Without the query string.
    path: []const u8,
    authorization: ?[]const u8,
    body: []const u8,
    keep_alive: bool,
    /// Bytes of the reader's buffer this request takes up (head and body).
    len: usize,
};

pub const ReadError = error{
    /// The peer closed the connection between requests.
    EndOfStream,
    ReadFailed,
    BadRequest,
    /// The request doesn't fit the read buffer.
    TooLarge,
};

/// Reads one request. Slices in the result point into the reader's buffer and
/// stay valid until the caller tosses `len` bytes. `w` receives a
/// "100 Continue" if the client asks for one.
pub fn readRequest(r: *Io.Reader, w: *Io.Writer) ReadError!Request {
    const head_len = try readHead(r);
    var head = parseHead(r.buffered()[0..head_len]) orelse return error.BadRequest;
    const total = head_len + head.content_length;
    if (total > r.buffer.len) return error.TooLarge;
    if (r.bufferedLen() < total and head.expect_continue) {
        w.writeAll("HTTP/1.1 100 Continue\r\n\r\n") catch return error.ReadFailed;
    }
    r.fill(total) catch |err| return switch (err) {
        error.EndOfStream => error.BadRequest,
        error.ReadFailed => error.ReadFailed,
    };
    // `fill` may have moved the buffered bytes: parse again in place.
    const bytes = r.buffered();
    head = parseHead(bytes[0..head_len]).?;
    return .{
        .method = head.method,
        .path = head.path,
        .authorization = head.authorization,
        .body = bytes[head_len..total],
        .keep_alive = head.keep_alive,
        .len = total,
    };
}

/// Returns the length of the head including the blank line.
fn readHead(r: *Io.Reader) ReadError!usize {
    var searched: usize = 0;
    while (true) {
        const bytes = r.buffered();
        // Scalar searches only: `indexOfPos` with a string reserves a 2 KB
        // skip table in its frame, a second stack page for every request.
        while (std.mem.indexOfScalarPos(u8, bytes, searched, '\n')) |i| {
            if (i >= 3 and std.mem.eql(u8, bytes[i - 3 .. i], "\r\n\r")) return i + 1;
            searched = i + 1;
        }
        searched = bytes.len;
        if (bytes.len == r.buffer.len) return error.TooLarge;
        r.fillMore() catch |err| return switch (err) {
            error.EndOfStream => if (bytes.len == 0) error.EndOfStream else error.BadRequest,
            error.ReadFailed => error.ReadFailed,
        };
    }
}

const Head = struct {
    method: Method,
    path: []const u8,
    authorization: ?[]const u8 = null,
    content_length: usize = 0,
    keep_alive: bool,
    expect_continue: bool = false,
};

fn parseHead(head: []const u8) ?Head {
    // Each line, without its "\r\n". As in `readHead`, split on a scalar.
    var lines = std.mem.splitScalar(u8, head[0 .. head.len - 3], '\n');
    const request_line = crlfLine(lines.first()) orelse return null;
    const sp1 = std.mem.indexOfScalar(u8, request_line, ' ') orelse return null;
    const sp2 = std.mem.lastIndexOfScalar(u8, request_line, ' ') orelse return null;
    if (sp2 <= sp1) return null;
    const method_text = request_line[0..sp1];
    const target = request_line[sp1 + 1 .. sp2];
    const version = request_line[sp2 + 1 ..];
    if (target.len == 0 or target[0] != '/') return null;
    const http11 = if (std.mem.eql(u8, version, "HTTP/1.1"))
        true
    else if (std.mem.eql(u8, version, "HTTP/1.0"))
        false
    else
        return null;
    var h: Head = .{
        .method = if (std.mem.eql(u8, method_text, "GET"))
            .get
        else if (std.mem.eql(u8, method_text, "POST"))
            .post
        else
            .other,
        .path = target[0 .. std.mem.indexOfScalar(u8, target, '?') orelse target.len],
        .keep_alive = http11,
    };
    var has_length = false;
    while (lines.next()) |crlf_line| {
        const line = crlfLine(crlf_line) orelse return null;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return null;
        const value = std.mem.trim(u8, line[colon + 1 ..], " \t");
        switch (headers.get(line[0..colon]) orelse continue) {
            .content_length => {
                if (has_length) return null;
                has_length = true;
                h.content_length = std.fmt.parseUnsigned(usize, value, 10) catch return null;
            },
            .authorization => h.authorization = value,
            .connection => {
                if (containsToken(value, "close")) h.keep_alive = false;
                if (containsToken(value, "keep-alive")) h.keep_alive = true;
            },
            .expect => h.expect_continue = std.ascii.eqlIgnoreCase(value, "100-continue"),
            .transfer_encoding => return null, // chunked bodies are not supported
        }
    }
    return h;
}

/// `line` without its trailing '\r', or null for a bare '\n' line ending.
fn crlfLine(line: []const u8) ?[]const u8 {
    if (line.len == 0 or line[line.len - 1] != '\r') return null;
    return line[0 .. line.len - 1];
}

/// The headers the server looks at. Names are case-insensitive.
const headers: std.StaticStringMapWithEql(enum {
    content_length,
    authorization,
    connection,
    expect,
    transfer_encoding,
}, std.static_string_map.eqlAsciiIgnoreCase) = .initComptime(.{
    .{ "content-length", .content_length },
    .{ "authorization", .authorization },
    .{ "connection", .connection },
    .{ "expect", .expect },
    .{ "transfer-encoding", .transfer_encoding },
});

fn containsToken(value: []const u8, comptime token: []const u8) bool {
    var it = std.mem.tokenizeAny(u8, value, ", \t");
    while (it.next()) |t| if (std.ascii.eqlIgnoreCase(t, token)) return true;
    return false;
}

test readRequest {
    const first = "POST /api/posts?x=1 HTTP/1.1\r\nHost: a\r\ncontent-length: 5\r\n" ++
        "AUTHORIZATION:  Bearer t \r\nConnection: close\r\n\r\nhello";
    var w: Io.Writer = .failing;
    var r: Io.Reader = .fixed(first ++ "GET /feed HTTP/1.0\r\n\r\n");
    const req = try readRequest(&r, &w);
    try std.testing.expectEqual(Method.post, req.method);
    try std.testing.expectEqualStrings("/api/posts", req.path);
    try std.testing.expectEqualStrings("Bearer t", req.authorization.?);
    try std.testing.expectEqualStrings("hello", req.body);
    try std.testing.expectEqual(false, req.keep_alive);
    try std.testing.expectEqual(first.len, req.len);
    r.toss(req.len);
    const next = try readRequest(&r, &w);
    try std.testing.expectEqualStrings("/feed", next.path);
    try std.testing.expectEqual(false, next.keep_alive);
    try std.testing.expectEqual(0, next.body.len);

    for ([_][]const u8{
        "GET / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n", // bare LF
        "GET / HTTP/1.1\nHost: a\r\n\r\n",
        "GET / HTTP/2\r\n\r\n",
        "GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
    }) |bad| {
        var br: Io.Reader = .fixed(bad);
        try std.testing.expectError(error.BadRequest, readRequest(&br, &w));
    }
}

const std = @import("std");
const Io = std.Io;
