//! $12 Server Challenge: high-performance in-process SQLite feed engine with Linux io_uring.
const std = @import("std");
const linux = std.os.linux;
const server = @import("server.zig");
const dbm = @import("db.zig");
const jwt = @import("jwt.zig");
const sys = @import("sys.zig");

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

pub fn main(init: std.process.Init) !void {
    // Ignore SIGPIPE so disconnected clients don't kill the server process
    const ignore: linux.Sigaction = .{
        .handler = .{ .handler = linux.SIG.IGN },
        .mask = linux.sigemptyset(),
        .flags = 0,
    };
    _ = linux.sigaction(.PIPE, &ignore, null);

    // Lock memory into RAM to eliminate page faults during benchmark
    sys.lockMemory();

    // Bump RLIMIT_NOFILE
    var rl: linux.rlimit = undefined;
    _ = linux.getrlimit(.NOFILE, &rl);
    rl.cur = rl.max;
    _ = linux.setrlimit(.NOFILE, &rl);

    // Environment configuration
    const sqlite_path = init.environ_map.get("SQLITE_PATH") orelse "seed/feed.db";
    const jwt_secret = init.environ_map.get("JWT_SECRET") orelse "twelve-dollar-challenge";
    const host_str = init.environ_map.get("HOST") orelse "0.0.0.0";
    const host = try parseHost(host_str);
    const port: u16 = @intCast(envInt(init, "PORT", 3000));

    // Calculate slots for io_uring direct descriptors
    const max_slots: u32 = @intCast(@min(rl.cur, 65535));
    const slots = @max(1024, max_slots);

    // Initialize database and JWT key
    const db = try dbm.Db.init(sqlite_path);
    const jwt_key = jwt.Key.init(jwt_secret);

    // Initialize server
    const s = try server.Server.init(db, jwt_key, slots);
    try s.makeListener(host, port);

    std.log.info("Server listening on {d}.{d}.{d}.{d}:{d} with {d} direct descriptors", .{
        host[0], host[1], host[2], host[3], port, slots,
    });

    try s.run();
}
