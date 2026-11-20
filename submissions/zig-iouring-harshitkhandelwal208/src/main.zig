//! $12 Server Challenge: high-performance in-process SQLite feed engine with Linux io_uring.
const std = @import("std");
const linux = std.os.linux;
const server = @import("server.zig");
const dbm = @import("db.zig");
const jwt = @import("jwt.zig");

pub const std_options: std.Options = .{ .log_level = .info };

fn parseHost(s: []const u8) ![4]u8 {
    if (std.mem.eql(u8, s, "localhost")) return [4]u8{ 127, 0, 0, 1 };
    var host: [4]u8 = undefined;
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |p| : (i += 1) {
        if (i >= 4) return error.BadHost;
        host[i] = try std.fmt.parseInt(u8, p, 10);
    }
    if (i != 4) return error.BadHost;
    return host;
}

fn envInt(init: std.process.Init, name: []const u8, default: u32) u32 {
    const v = init.environ_map.get(name) orelse return default;
    return std.fmt.parseInt(u32, v, 10) catch default;
}

fn termHandler(sig: linux.SIG, info: *const linux.siginfo_t, ctx: ?*anyopaque) callconv(.c) void {
    _ = sig;
    _ = info;
    _ = ctx;
    server.Server.running.store(false, .monotonic);
}

fn crashHandler(sig: linux.SIG, info: *const linux.siginfo_t, ctx: ?*anyopaque) callconv(.c) void {
    _ = ctx;
    const name: []const u8 = switch (sig) {
        .SEGV => "SIGSEGV (segmentation fault)\n",
        .BUS => "SIGBUS (bus error)\n",
        .ILL => "SIGILL (illegal instruction)\n",
        .FPE => "SIGFPE (floating point exception)\n",
        .ABRT => "SIGABRT (aborted)\n",
        else => "UNKNOWN SIGNAL\n",
    };
    const prefix = "FATAL CRASH: caught ";
    _ = linux.write(2, prefix.ptr, prefix.len);
    _ = linux.write(2, name.ptr, name.len);

    var buf: [64]u8 = undefined;
    const addr = @intFromPtr(info.fields.sigfault.addr);
    const msg = std.fmt.bufPrint(&buf, "Faulting address: 0x{x}, code: {d}\n", .{ addr, info.code }) catch "bad addr\n";
    _ = linux.write(2, msg.ptr, msg.len);

    linux.exit_group(128 + @as(u8, @intCast(@intFromEnum(sig) & 0x7f)));
}

pub fn main(init: std.process.Init) !void {
    // Install signal crash handler for fatal crashes only.
    // SIGTERM and SIGINT are left at default action so the kernel cleanly terminates all threads on shutdown.
    const sa: linux.Sigaction = .{
        .handler = .{ .sigaction = crashHandler },
        .mask = linux.sigemptyset(),
        .flags = linux.SA.SIGINFO,
    };
    _ = linux.sigaction(.SEGV, &sa, null);
    _ = linux.sigaction(.BUS, &sa, null);
    _ = linux.sigaction(.ILL, &sa, null);
    _ = linux.sigaction(.FPE, &sa, null);
    _ = linux.sigaction(.ABRT, &sa, null);

    const term_sa: linux.Sigaction = .{
        .handler = .{ .sigaction = termHandler },
        .mask = linux.sigemptyset(),
        .flags = linux.SA.SIGINFO,
    };
    _ = linux.sigaction(.INT, &term_sa, null);
    _ = linux.sigaction(.TERM, &term_sa, null);

    // Ignore SIGPIPE so disconnected clients don't kill the server process
    const ignore: linux.Sigaction = .{
        .handler = .{ .handler = linux.SIG.IGN },
        .mask = linux.sigemptyset(),
        .flags = 0,
    };
    _ = linux.sigaction(.PIPE, &ignore, null);

    // Bump RLIMIT_NOFILE and RLIMIT_MEMLOCK early before any ring or descriptor allocations
    var rl: linux.rlimit = undefined;
    _ = linux.getrlimit(.NOFILE, &rl);
    rl.cur = rl.max;
    _ = linux.setrlimit(.NOFILE, &rl);

    var rl_mem: linux.rlimit = undefined;
    if (linux.getrlimit(.MEMLOCK, &rl_mem) == 0) {
        rl_mem.cur = rl_mem.max;
        _ = linux.setrlimit(.MEMLOCK, &rl_mem);
    }

    // Environment configuration
    const sqlite_path = init.environ_map.get("SQLITE_PATH") orelse "seed/feed.db";
    const jwt_secret = init.environ_map.get("JWT_SECRET") orelse "twelve-dollar-challenge";
    const host_str = init.environ_map.get("HOST") orelse "0.0.0.0";
    const host = try parseHost(host_str);
    const port: u16 = @intCast(envInt(init, "PORT", 3000));

    // Initialize JWT key and multi-ring server (8 rings x 65,472 = 523,776 direct descriptors)
    const jwt_key = jwt.Key.init(jwt_secret);
    const s = try server.Server.init(sqlite_path, jwt_key);
    try s.makeListeners(host, port);

    std.log.info("Server listening on {d}.{d}.{d}.{d}:{d} with {d} rings x {d} slots = {d} direct descriptors", .{
        host[0], host[1], host[2], host[3], port, s.active_rings, server.slots_per_ring, s.active_rings * server.slots_per_ring,
    });

    try s.run();
}
