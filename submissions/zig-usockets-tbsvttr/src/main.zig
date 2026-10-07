const std = @import("std");
const c = @import("c.zig").lib;
const app = @import("app.zig");
const alloc = app.alloc;
const Socket = c.struct_us_socket_t;
const Connection = struct {
    input: std.ArrayList(u8) = .empty,
    output: std.ArrayList(u8) = .empty,
    sent: usize = 0,
    waiting: usize = 0,
    closing: bool = false,
    continued: bool = false,
};
const Pending = struct { socket: ?*Socket, status: u16, body: []u8 };
var pending: std.ArrayList(Pending) = .empty;

fn connection(s: ?*Socket) *Connection {
    return @ptrCast(@alignCast(c.us_socket_ext(0, s)));
}

fn close(s: ?*Socket) ?*Socket {
    return c.us_socket_close(0, s, 0, null);
}

fn maybeClose(s: ?*Socket) void {
    const conn = connection(s);
    if (conn.closing and conn.waiting == 0 and conn.sent == conn.output.items.len) _ = close(s);
}

fn opened(s: ?*Socket, _: c_int, _: [*c]u8, _: c_int) callconv(.c) ?*Socket {
    connection(s).* = .{};
    c.us_socket_timeout(0, s, 75);
    return s;
}

fn closed(s: ?*Socket, _: c_int, _: ?*anyopaque) callconv(.c) ?*Socket {
    for (pending.items) |*item| if (item.socket == s) {
        item.socket = null;
    };
    const conn = connection(s);
    conn.input.deinit(alloc);
    conn.output.deinit(alloc);
    return s;
}

fn ended(s: ?*Socket) callconv(.c) ?*Socket {
    connection(s).closing = true;
    maybeClose(s);
    return s;
}

fn timeout(s: ?*Socket) callconv(.c) ?*Socket {
    return close(s);
}

fn writable(s: ?*Socket) callconv(.c) ?*Socket {
    const conn = connection(s);
    const bytes = conn.output.items[conn.sent..];
    if (bytes.len != 0) {
        const n = c.us_socket_write(0, s, bytes.ptr, @intCast(bytes.len), 0);
        if (n > 0) conn.sent += @intCast(n);
    }
    if (conn.sent == conn.output.items.len) {
        conn.output.clearRetainingCapacity();
        conn.sent = 0;
        maybeClose(s);
    }
    return s;
}

fn wire(s: ?*Socket, status: u16, body: []const u8) !void {
    const conn = connection(s);
    var buf: [256]u8 = undefined;
    const reason: []const u8 = switch (status) {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        413 => "Payload Too Large",
        417 => "Expectation Failed",
        503 => "Service Unavailable",
        else => "Internal Server Error",
    };
    const header = if (status == 100) "HTTP/1.1 100 Continue\r\n\r\n" else try std.fmt.bufPrint(&buf, "HTTP/1.1 {d} {s}\r\nContent-Type: application/json\r\nContent-Length: {d}\r\nConnection: {s}\r\n\r\n", .{ status, reason, body.len, if (conn.closing) @as([]const u8, "close") else "keep-alive" });
    var written: usize = 0;
    if (conn.sent == conn.output.items.len) {
        const n = c.us_socket_write2(0, s, header.ptr, @intCast(header.len), body.ptr, @intCast(body.len));
        if (n > 0) written = @intCast(n);
    }
    if (written == header.len + body.len) return;
    if (conn.sent != 0) {
        const rest = conn.output.items.len - conn.sent;
        std.mem.copyForwards(u8, conn.output.items[0..rest], conn.output.items[conn.sent..]);
        conn.output.items.len = rest;
        conn.sent = 0;
    }
    if (conn.output.items.len + header.len + body.len > 8 * 1024 * 1024) return error.SlowClient;
    if (written < header.len) try conn.output.appendSlice(alloc, header[written..]);
    try conn.output.appendSlice(alloc, body[written -| header.len..]);
}

fn deliver(s: ?*Socket, result: app.Response) void {
    if (app.transaction) {
        const body = alloc.dupe(u8, result.body) catch {
            _ = close(s);
            return;
        };
        pending.append(alloc, .{ .socket = s, .status = result.status, .body = body }) catch {
            alloc.free(body);
            _ = close(s);
            return;
        };
        connection(s).waiting += 1;
        if (pending.items.len >= 256) flush();
    } else {
        wire(s, result.status, result.body) catch {
            _ = close(s);
            return;
        };
        maybeClose(s);
    }
}

fn flush() void {
    if (!app.transaction) return;
    const committed = app.commit();
    for (pending.items) |item| {
        defer alloc.free(item.body);
        if (item.socket) |s| {
            connection(s).waiting -= 1;
            const result = if (committed) app.Response{ .status = item.status, .body = item.body } else app.internal;
            wire(s, result.status, result.body) catch {
                _ = close(s);
                continue;
            };
            maybeClose(s);
        }
    }
    pending.clearRetainingCapacity();
}

fn noop(_: ?*c.struct_us_loop_t) callconv(.c) void {}
fn afterLoop(_: ?*c.struct_us_loop_t) callconv(.c) void {
    flush();
}

fn equal(a: []const u8, b: []const u8) bool {
    return std.ascii.eqlIgnoreCase(a, b);
}
fn trim(value: []const u8) []const u8 {
    return std.mem.trim(u8, value, " \t");
}

fn token(list: []const u8, wanted: []const u8) bool {
    var it = std.mem.splitScalar(u8, list, ',');
    while (it.next()) |part| if (equal(trim(part), wanted)) return true;
    return false;
}

fn bad(s: ?*Socket, status: u16) void {
    connection(s).closing = true;
    deliver(s, .{ .status = status, .body = "{\"error\":\"invalid HTTP request\"}" });
}

fn data(s: ?*Socket, bytes: [*c]u8, len: c_int) callconv(.c) ?*Socket {
    const conn = connection(s);
    if (conn.closing) return s;
    c.us_socket_timeout(0, s, 75);
    if (conn.input.items.len + @as(usize, @intCast(len)) > 512 * 1024) {
        bad(s, 413);
        return s;
    }
    conn.input.appendSlice(alloc, bytes[0..@intCast(len)]) catch return close(s);
    var consumed: usize = 0;
    while (consumed < conn.input.items.len) {
        const input = conn.input.items[consumed..];
        var method: [*c]const u8 = null;
        var path: [*c]const u8 = null;
        var method_len: usize = 0;
        var path_len: usize = 0;
        var minor: c_int = 0;
        var headers: [64]c.struct_phr_header = undefined;
        var count: usize = headers.len;
        const parsed = c.phr_parse_request(input.ptr, input.len, &method, &method_len, &path, &path_len, &minor, &headers, &count, 0);
        if (parsed == -2 and input.len <= 32768) break;
        if (parsed < 0 or parsed > 32768 or minor < 0 or minor > 1) {
            bad(s, 400);
            return s;
        }
        const header_len: usize = @intCast(parsed);
        var length: ?usize = null;
        var chunked = false;
        var auth: ?[]const u8 = null;
        var host = false;
        var expect = false;
        var closing = minor == 0;
        for (headers[0..count]) |h| {
            if (h.name == null) {
                bad(s, 400);
                return s;
            }
            const name = h.name[0..h.name_len];
            const value = trim(h.value[0..h.value_len]);
            if (equal(name, "content-length")) {
                if (length != null or value.len == 0) {
                    bad(s, 400);
                    return s;
                }
                for (value) |ch| if (ch < '0' or ch > '9') {
                    bad(s, 400);
                    return s;
                };
                length = std.fmt.parseInt(usize, value, 10) catch {
                    bad(s, 400);
                    return s;
                };
            } else if (equal(name, "transfer-encoding")) {
                if (chunked or !equal(value, "chunked")) {
                    bad(s, 400);
                    return s;
                }
                chunked = true;
            } else if (equal(name, "authorization")) {
                if (auth != null) {
                    bad(s, 400);
                    return s;
                }
                auth = value;
            } else if (equal(name, "host")) {
                if (host or value.len == 0) {
                    bad(s, 400);
                    return s;
                }
                host = true;
            } else if (equal(name, "connection")) {
                if (token(value, "keep-alive") and minor == 0) closing = false;
                if (token(value, "close")) closing = true;
            } else if (equal(name, "expect")) {
                if (!equal(value, "100-continue")) {
                    bad(s, 417);
                    return s;
                }
                expect = true;
            }
        }
        if ((minor == 1 and !host) or (chunked and length != null)) {
            bad(s, 400);
            return s;
        }
        if ((length orelse 0) > 16384) {
            bad(s, 413);
            return s;
        }
        var body = input[header_len..];
        var used: usize = header_len + (length orelse 0);
        var decoded: ?[]u8 = null;
        defer if (decoded) |memory| alloc.free(memory);
        var incomplete = used > input.len;
        if (chunked) {
            if (body.len > 128 * 1024) {
                bad(s, 413);
                return s;
            }
            decoded = alloc.dupe(u8, body) catch return close(s);
            var decoder: c.struct_phr_chunked_decoder = std.mem.zeroes(c.struct_phr_chunked_decoder);
            decoder.consume_trailer = 1;
            var size = body.len;
            const rc = c.phr_decode_chunked(&decoder, decoded.?.ptr, &size);
            if (rc == -1) {
                bad(s, 400);
                return s;
            }
            if (size > 16384) {
                bad(s, 413);
                return s;
            }
            incomplete = rc == -2;
            if (!incomplete) {
                used = input.len - @as(usize, @intCast(rc));
                body = decoded.?[0..size];
            }
        } else if (!incomplete) body = input[header_len..used];
        if (incomplete) {
            if (expect and !conn.continued) {
                conn.continued = true;
                deliver(s, .{ .status = 100, .body = "" });
                if (c.us_socket_is_closed(0, s) != 0) return s;
            }
            break;
        }
        conn.continued = false;
        conn.closing = closing;
        deliver(s, app.handle(method[0..method_len], path[0..path_len], auth orelse "", body));
        if (c.us_socket_is_closed(0, s) != 0 or conn.closing) return s;
        consumed += used;
    }
    const rest = conn.input.items.len - consumed;
    std.mem.copyForwards(u8, conn.input.items[0..rest], conn.input.items[consumed..]);
    conn.input.items.len = rest;
    return s;
}

pub fn main() !void {
    try app.init();
    var limit: c.struct_rlimit = undefined;
    if (c.getrlimit(c.RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        _ = c.setrlimit(c.RLIMIT_NOFILE, &limit);
    }
    const loop = c.us_create_loop(null, noop, noop, afterLoop, 0) orelse return error.EventLoop;
    const context = c.us_create_socket_context(0, loop, 0, std.mem.zeroes(c.struct_us_socket_context_options_t)) orelse return error.SocketContext;
    c.us_socket_context_on_open(0, context, opened);
    c.us_socket_context_on_close(0, context, closed);
    c.us_socket_context_on_data(0, context, data);
    c.us_socket_context_on_writable(0, context, writable);
    c.us_socket_context_on_timeout(0, context, timeout);
    c.us_socket_context_on_end(0, context, ended);
    const port = try std.fmt.parseInt(c_int, app.env("PORT", "3000"), 10);
    if (port < 1 or port > 65535) return error.InvalidPort;
    if (c.us_socket_context_listen(0, context, app.env("HOST", "127.0.0.1").ptr, port, 0, @sizeOf(Connection)) == null) return error.Listen;
    c.us_loop_run(loop);
}
