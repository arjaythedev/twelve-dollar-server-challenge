//! The $12 server: plain `std.Io` code (accept, read, write, group, event)
//! running on `UringIo`, a single-threaded io_uring + fibers implementation.

pub const std_options: std.Options = .{ .io_vtable = .{ .custom = UringIo } };

pub const std_options_debug_io: Io = UringIo.ioFromInstance();

pub fn main(init: std.process.Init.Minimal) !void {
    const env = init.environ;
    const sqlite_path = env.getPosix("SQLITE_PATH") orelse fatal("SQLITE_PATH is not set", .{});
    const secret = env.getPosix("JWT_SECRET") orelse fatal("JWT_SECRET is not set", .{});
    const host = env.getPosix("HOST") orelse "127.0.0.1";
    const port_text = env.getPosix("PORT") orelse "3000";
    const port = std.fmt.parseUnsigned(u16, port_text, 10) catch fatal("bad PORT '{s}'", .{port_text});
    const address = net.IpAddress.parse(host, port) catch fatal("bad HOST '{s}'", .{host});

    const max_fds = raiseFdLimit();
    UringIo.instance.init(.{
        .max_fibers = @intCast(@min(max_fds + 64, 1 << 18)),
        // At 64k users, batching completions took CPU from 23.3% to 19.0% for
        // ~0.4 ms of latency (a 1 ms cap saved no more, 0.25 ms saved less).
        .batch = .{ .size = 32, .wait_ns = 500 * std.time.ns_per_us },
    }) catch |err|
        fatal("io_uring setup failed: {t}", .{err});
    const io = UringIo.ioFromInstance();

    var app: App = undefined;
    app.init(io, sqlite_path, secret) catch |err| fatal("startup failed: {t}", .{err});

    UringIo.instance.run(serve, .{ &app, address });
}

fn serve(app: *App, address: net.IpAddress) void {
    const io = app.io;
    var server = address.listen(io, .{ .reuse_address = true, .kernel_backlog = 4096 }) catch |err|
        fatal("listen on {f}: {t}", .{ address, err });
    std.log.info("listening on {f}", .{server.socket.address});

    var group: Io.Group = .init;
    group.concurrent(io, App.committer, .{app}) catch fatal("no task for the committer", .{});
    while (true) {
        const stream = server.accept(io) catch |err| {
            std.log.err("accept: {t}", .{err});
            continue;
        };
        group.concurrent(io, serveConnection, .{ app, stream }) catch {
            stream.close(io);
        };
    }
}

fn serveConnection(app: *App, stream: net.Stream) void {
    const io = app.io;
    defer stream.close(io);
    // Small, so that this connection's whole live stack fits in its slot's
    // top page: resuming it faults in one page, not two. A request that
    // doesn't fit moves to a heap buffer. While the connection is idle the
    // pending read owns the buffer, so parking skips it.
    var small_buffer: [2048]u8 = undefined;
    var big_buffer: ?[]u8 = null;
    defer if (big_buffer) |b| std.heap.c_allocator.free(b);
    var reader = stream.reader(io, &small_buffer);
    var writer = stream.writer(io, &.{});
    while (true) {
        const req = http.readRequest(&reader.interface, &writer.interface) catch |err| {
            if (err == error.TooLarge and big_buffer == null) {
                big_buffer = growBuffer(&reader.interface) orelse return;
                continue;
            }
            const response: []const u8 = switch (err) {
                error.EndOfStream, error.ReadFailed => return,
                error.BadRequest => "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                error.TooLarge => "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            };
            writer.interface.writeAll(response) catch {};
            return;
        };
        if (req.method == .post) app.waitWritable();
        const response = UringIo.callOnSchedulerStack(io, App.handle, .{ app, &req });
        // The response is in a buffer every request shares; own it before suspending.
        const owned = std.heap.c_allocator.dupe(u8, response.bytes) catch return;
        defer std.heap.c_allocator.free(owned);
        const bytes = if (response.batch) |batch|
            if (app.awaitCommit(batch)) owned else App.internalError(req.keep_alive)
        else
            owned;
        writer.interface.writeAll(bytes) catch return;
        reader.interface.toss(req.len);
        if (!req.keep_alive) return;
    }
}

/// Moves `r`'s buffered bytes to a heap buffer that fits any request.
fn growBuffer(r: *Io.Reader) ?[]u8 {
    const big = std.heap.c_allocator.alloc(u8, http.max_request) catch return null;
    const buffered = r.buffered();
    @memcpy(big[0..buffered.len], buffered);
    r.buffer = big;
    r.seek = 0;
    r.end = buffered.len;
    return big;
}

/// Raises the soft open-file limit to the hard one. Returns the limit.
fn raiseFdLimit() usize {
    const linux = std.os.linux;
    var lim: linux.rlimit = undefined;
    if (linux.errno(linux.getrlimit(.NOFILE, &lim)) != .SUCCESS) fatal("getrlimit failed", .{});
    if (lim.cur != lim.max) {
        const soft = lim.cur;
        lim.cur = lim.max;
        if (linux.errno(linux.setrlimit(.NOFILE, &lim)) != .SUCCESS) {
            std.log.warn("cannot raise the open file limit from {d} to {d}", .{ soft, lim.max });
            lim.cur = soft;
        }
    }
    return @intCast(lim.cur);
}

fn fatal(comptime fmt: []const u8, args: anytype) noreturn {
    std.log.err(fmt, args);
    std.process.exit(1);
}

const std = @import("std");
const Io = std.Io;
const net = Io.net;
const App = @import("App.zig");
const http = @import("http.zig");
const UringIo = @import("UringIo.zig");
