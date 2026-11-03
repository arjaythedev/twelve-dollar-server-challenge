//! A load generator that follows bench/load.js, one fiber per virtual user,
//! on the same UringIo as the server. A VU costs a few KB instead of k6's
//! ~300 KB, so one box can hold 100k+ keep-alive connections.
//!
//!   loadgen --vus 64000 [--port 3000] [--ramp 60] [--duration 300]
//!           [--tokens ../../seed/tokens.json] [--dests 4] [--pid SERVER_PID]
//!
//! The server should listen on 0.0.0.0: VU i connects to 127.0.0.(1 + i % dests),
//! each destination address giving another ~28k ephemeral ports.
//!
//! VU loop (bench/load.js): GET /feed -> think 3-7 s -> GET /posts/:id (from the
//! feed) -> think 3-8 s -> like it (15%) -> create a post (2%) -> idle 5-15 s.
//! Latency is from writing the request to reading the whole response, like
//! k6's http_req_duration on a reused connection.

pub const std_options: std.Options = .{ .io_vtable = .{ .custom = UringIo } };

pub const std_options_debug_io: Io = UringIo.ioFromInstance();

const Config = struct {
    vus: u32 = 1000,
    port: u16 = 3000,
    ramp_s: u32 = 60,
    duration_s: u32 = 300,
    dests: u32 = 0,
    tokens_path: []const u8 = "../../seed/tokens.json",
    server_pid: ?u32 = null,
    interval_s: u32 = 10,
};

const Token = struct { username: []const u8, token: []const u8 };

const Endpoint = enum { feed, post, like, create };

var config: Config = .{};
var tokens: []Token = &.{};
var stats: Stats = .{};

pub fn main(init: std.process.Init.Minimal) !void {
    var args = init.args.iterate();
    _ = args.skip();
    while (args.next()) |arg| {
        const value = args.next() orelse fatal("missing value for {s}", .{arg});
        if (std.mem.eql(u8, arg, "--vus")) {
            config.vus = try std.fmt.parseUnsigned(u32, value, 10);
        } else if (std.mem.eql(u8, arg, "--port")) {
            config.port = try std.fmt.parseUnsigned(u16, value, 10);
        } else if (std.mem.eql(u8, arg, "--ramp")) {
            config.ramp_s = try std.fmt.parseUnsigned(u32, value, 10);
        } else if (std.mem.eql(u8, arg, "--duration")) {
            config.duration_s = try std.fmt.parseUnsigned(u32, value, 10);
        } else if (std.mem.eql(u8, arg, "--dests")) {
            config.dests = try std.fmt.parseUnsigned(u32, value, 10);
        } else if (std.mem.eql(u8, arg, "--tokens")) {
            config.tokens_path = value;
        } else if (std.mem.eql(u8, arg, "--pid")) {
            config.server_pid = try std.fmt.parseUnsigned(u32, value, 10);
        } else if (std.mem.eql(u8, arg, "--interval")) {
            config.interval_s = try std.fmt.parseUnsigned(u32, value, 10);
        } else fatal("unknown option {s}", .{arg});
    }
    if (config.dests == 0) config.dests = config.vus / 25_000 + 1;

    var lim: std.os.linux.rlimit = undefined;
    _ = std.os.linux.getrlimit(.NOFILE, &lim);
    lim.cur = lim.max;
    _ = std.os.linux.setrlimit(.NOFILE, &lim);
    if (lim.cur < config.vus + 64) fatal("open file limit {d} is too low for {d} VUs", .{ lim.cur, config.vus });

    UringIo.instance.init(.{ .max_fibers = config.vus + 16 }) catch |err| fatal("io_uring: {t}", .{err});
    const io = UringIo.ioFromInstance();
    tokens = loadTokens(io) catch |err| fatal("reading {s}: {t}", .{ config.tokens_path, err });
    UringIo.instance.run(run, .{io});
}

fn loadTokens(io: Io) ![]Token {
    const gpa = std.heap.c_allocator;
    const text = try Io.Dir.cwd().readFileAlloc(io, config.tokens_path, gpa, .limited(64 << 20));
    var list: std.ArrayList(Token) = .empty;
    var rest: []const u8 = text;
    while (field(&rest, "\"username\":\"")) |username| {
        const token = field(&rest, "\"token\":\"") orelse return error.BadTokens;
        try list.append(gpa, .{ .username = username, .token = token });
    }
    if (list.items.len == 0) return error.NoTokens;
    return list.items;
}

fn field(rest: *[]const u8, comptime prefix: []const u8) ?[]const u8 {
    const start = (std.mem.indexOf(u8, rest.*, prefix) orelse return null) + prefix.len;
    const end = std.mem.indexOfScalarPos(u8, rest.*, start, '"') orelse return null;
    const value = rest.*[start..end];
    rest.* = rest.*[end..];
    return value;
}

fn run(io: Io) void {
    var group: Io.Group = .init;
    const start = Io.Timestamp.now(io, .awake);
    stats.start_ns = start.nanoseconds;
    group.concurrent(io, report, .{io}) catch fatal("no task for the reporter", .{});
    std.log.info("{d} VUs over {d} destination addresses, ramp {d}s, hold {d}s", .{ config.vus, config.dests, config.ramp_s, config.duration_s });
    // Start VUs evenly over the ramp, in 10 ms steps.
    var started: u32 = 0;
    const ramp_ns: i96 = @as(i96, config.ramp_s) * std.time.ns_per_s;
    while (started < config.vus) {
        const elapsed = Io.Timestamp.now(io, .awake).nanoseconds - start.nanoseconds;
        const due: u32 = if (ramp_ns == 0) config.vus else @intCast(@min(config.vus, @divFloor(elapsed * config.vus, ramp_ns)));
        while (started < due) : (started += 1) {
            group.concurrent(io, vu, .{ io, started }) catch fatal("no task for VU {d}", .{started});
        }
        stats.vus = started;
        if (started < config.vus) io.sleep(.fromMilliseconds(10), .awake) catch {};
    }
    io.sleep(.fromSeconds(config.duration_s), .awake) catch {};
    stats.printSummary();
    std.process.exit(0);
}

const Prng = std.Random.DefaultPrng;

fn vu(io: Io, index: u32) void {
    var prng: Prng = .init(@as(u64, index) *% 0x9e3779b97f4a7c15 +% 12345);
    const rand = prng.random();
    const tok = tokens[index % tokens.len];
    const dest: net.IpAddress = .{ .ip4 = .{
        .bytes = .{ 127, 0, @intCast((1 + index % config.dests) >> 8), @intCast((1 + index % config.dests) & 0xff) },
        .port = config.port,
    } };
    var auth_buf: [600]u8 = undefined;
    const auth = std.fmt.bufPrint(&auth_buf, "Authorization: Bearer {s}\r\n", .{tok.token}) catch unreachable;

    sleepBetween(io, rand, 0, 5);
    var iter: u64 = 0;
    while (true) : (iter += 1) {
        var conn = Conn.open(io, &dest) orelse {
            sleepBetween(io, rand, 1, 2);
            continue;
        };
        defer {
            conn.stream.close(io);
            std.heap.c_allocator.destroy(conn);
        }
        while (true) : (iter += 1) {
            const feed = conn.request(.feed, "GET /feed HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: loadgen\r\n\r\n", "", "", 200, 200) orelse break;
            const id = pickPostId(feed, rand) orelse {
                stats.fail(.feed);
                sleepBetween(io, rand, 3, 7);
                continue;
            };
            sleepBetween(io, rand, 3, 7);
            var head_buf: [128]u8 = undefined;
            const get = std.fmt.bufPrint(&head_buf, "GET /posts/{d} HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: loadgen\r\n\r\n", .{id}) catch unreachable;
            _ = conn.request(.post, get, "", "", 200, 200) orelse break;
            sleepBetween(io, rand, 3, 8);
            if (rand.float(f64) < 0.15) {
                const like = std.fmt.bufPrint(&head_buf, "POST /posts/{d}/like HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: loadgen\r\nContent-Length: 0\r\n", .{id}) catch unreachable;
                _ = conn.request(.like, like, auth, "", 200, 201) orelse break;
            }
            if (rand.float(f64) < 0.02) {
                var body_buf: [256]u8 = undefined;
                const body = std.fmt.bufPrint(&body_buf, "{{\"body\":\"{s} says hi (VU {d}, iter {d})\"}}", .{ tok.username, index + 1, iter }) catch unreachable;
                var create_buf: [256]u8 = undefined;
                const create = std.fmt.bufPrint(&create_buf, "POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: loadgen\r\nContent-Type: application/json\r\nContent-Length: {d}\r\n", .{body.len}) catch unreachable;
                _ = conn.request(.create, create, auth, body, 201, 201) orelse break;
            }
            sleepBetween(io, rand, 5, 15);
        }
    }
}

fn sleepBetween(io: Io, rand: std.Random, lo: f64, hi: f64) void {
    const s = lo + rand.float(f64) * (hi - lo);
    io.sleep(.fromNanoseconds(@intFromFloat(s * std.time.ns_per_s)), .awake) catch {};
}

/// Picks a random `"id":N` of the feed's posts.
fn pickPostId(body: []const u8, rand: std.Random) ?u64 {
    var ids: [20]u64 = undefined;
    var n: usize = 0;
    var i: usize = 0;
    while (n < ids.len) {
        const at = std.mem.indexOfPos(u8, body, i, "{\"id\":") orelse break;
        var end = at + 6;
        while (end < body.len and std.ascii.isDigit(body[end])) end += 1;
        ids[n] = std.fmt.parseUnsigned(u64, body[at + 6 .. end], 10) catch return null;
        n += 1;
        i = end;
    }
    if (n == 0) return null;
    return ids[rand.uintLessThan(usize, n)];
}

const Conn = struct {
    io: Io,
    stream: net.Stream,
    reader: net.Stream.Reader,
    writer: net.Stream.Writer,
    read_buffer: [16 * 1024]u8,

    fn open(io: Io, dest: *const net.IpAddress) ?*Conn {
        const stream = dest.connect(io, .{ .mode = .stream }) catch |err| {
            stats.connectFailed(err);
            return null;
        };
        const conn = std.heap.c_allocator.create(Conn) catch @panic("OOM");
        conn.* = .{ .io = io, .stream = stream, .reader = undefined, .writer = undefined, .read_buffer = undefined };
        conn.reader = stream.reader(io, &conn.read_buffer);
        conn.writer = stream.writer(io, &.{});
        stats.connections += 1;
        return conn;
    }

    /// Sends `head ++ extra_header ++ "\r\n" ++ body`, returns the response
    /// body (valid until the next request), or null after a failure (the
    /// connection is then unusable).
    fn request(c: *Conn, ep: Endpoint, head: []const u8, extra_header: []const u8, body: []const u8, ok1: u16, ok2: u16) ?[]const u8 {
        const io = c.io;
        const head_end = if (std.mem.endsWith(u8, head, "\r\n\r\n")) "" else "\r\n";
        const t0 = Io.Timestamp.now(io, .awake).nanoseconds;
        var parts = [_][]const u8{ head, extra_header, head_end, body };
        c.writer.interface.writeVecAll(&parts) catch return c.failed(ep);
        const r = &c.reader.interface;
        // Head.
        var searched: usize = 0;
        const head_len = while (true) {
            const buf = r.buffered();
            if (std.mem.indexOfPos(u8, buf, searched, "\r\n\r\n")) |i| break i + 4;
            searched = buf.len -| 3;
            r.fillMore() catch return c.failed(ep);
        };
        const resp_head = r.buffered()[0..head_len];
        if (resp_head.len < 12) return c.failed(ep);
        const status = std.fmt.parseUnsigned(u16, resp_head[9..12], 10) catch return c.failed(ep);
        const cl_at = std.mem.indexOf(u8, resp_head, "Content-Length:") orelse return c.failed(ep);
        var cl_end = cl_at + "Content-Length:".len;
        while (resp_head[cl_end] == ' ') cl_end += 1;
        const cl_start = cl_end;
        while (std.ascii.isDigit(resp_head[cl_end])) cl_end += 1;
        const len = std.fmt.parseUnsigned(usize, resp_head[cl_start..cl_end], 10) catch return c.failed(ep);
        r.fill(head_len + len) catch return c.failed(ep);
        const t1 = Io.Timestamp.now(io, .awake).nanoseconds;
        const resp_body = r.buffered()[head_len..][0..len];
        r.toss(head_len + len);
        stats.record(ep, @intCast(t1 - t0), status == ok1 or status == ok2);
        return resp_body;
    }

    /// The caller then leaves its request loop, which closes and frees `c`.
    fn failed(c: *Conn, ep: Endpoint) ?[]const u8 {
        _ = c;
        stats.fail(ep);
        stats.connections -= 1;
        return null;
    }
};

// ---------------------------------------------------------------------------
// Statistics

/// Log-linear latency histogram: 16 buckets per power of two of microseconds.
const Histogram = struct {
    const sub = 16;
    const buckets = 40 * sub;
    counts: [buckets]u64 = @splat(0),
    total: u64 = 0,
    max_us: u64 = 0,
    sum_us: u64 = 0,

    fn bucket(us: u64) usize {
        if (us < sub) return us;
        const log2: usize = 63 - @clz(us);
        const shift: u6 = @intCast(log2 - 4);
        const mant: usize = @intCast((us >> shift) - sub);
        return @min(buckets - 1, (log2 - 3) * sub + mant);
    }

    fn lowerBound(b: usize) u64 {
        if (b < sub) return b;
        const log2 = b / sub + 3;
        const mant = b % sub;
        return @as(u64, sub + mant) << @intCast(log2 - 4);
    }

    fn add(h: *Histogram, us: u64) void {
        h.counts[bucket(us)] += 1;
        h.total += 1;
        h.sum_us += us;
        h.max_us = @max(h.max_us, us);
    }

    fn merge(h: *Histogram, o: *const Histogram) void {
        for (&h.counts, o.counts) |*a, b| a.* += b;
        h.total += o.total;
        h.sum_us += o.sum_us;
        h.max_us = @max(h.max_us, o.max_us);
    }

    fn percentile(h: *const Histogram, p: f64) f64 {
        if (h.total == 0) return 0;
        const rank: u64 = @intFromFloat(@ceil(p * @as(f64, @floatFromInt(h.total))));
        var seen: u64 = 0;
        for (h.counts, 0..) |count, b| {
            seen += count;
            if (seen >= rank) return @as(f64, @floatFromInt(lowerBound(b + 1))) / 1000.0;
        }
        return @as(f64, @floatFromInt(h.max_us)) / 1000.0;
    }
};

const Stats = struct {
    start_ns: i96 = 0,
    vus: u32 = 0,
    connections: u32 = 0,
    connect_failures: u64 = 0,
    interval: Histogram = .{},
    /// From the end of the ramp, what k6's thresholds would judge.
    hold: Histogram = .{},
    interval_failed: u64 = 0,
    hold_failed: u64 = 0,
    per_endpoint: [4]Histogram = @splat(.{}),
    cpu_hold_start: ?f64 = null,
    hold_start_s: f64 = 0,
    max_rss_kb: u64 = 0,

    fn record(s: *Stats, ep: Endpoint, ns: u64, ok: bool) void {
        const us = ns / 1000;
        s.interval.add(us);
        if (!ok) s.interval_failed += 1;
        if (s.vus == config.vus) {
            s.hold.add(us);
            s.per_endpoint[@backingInt(ep)].add(us);
            if (!ok) s.hold_failed += 1;
        }
    }

    fn fail(s: *Stats, ep: Endpoint) void {
        _ = ep;
        s.interval_failed += 1;
        if (s.vus == config.vus) s.hold_failed += 1;
    }

    fn connectFailed(s: *Stats, err: anyerror) void {
        if (s.connect_failures < 5) std.log.err("connect: {t}", .{err});
        s.connect_failures += 1;
    }

    fn printSummary(s: *Stats) void {
        const h = &s.hold;
        const failed_pct = if (h.total + s.hold_failed == 0) 0 else 100.0 * @as(f64, @floatFromInt(s.hold_failed)) / @as(f64, @floatFromInt(h.total + s.hold_failed));
        std.debug.print(
            \\== hold ({d} VUs): {d} requests, {d} failed ({d:.3}%), avg {d:.1} ms, p50 {d:.1} ms, p95 {d:.1} ms, p99 {d:.1} ms, max {d:.1} ms
            \\
        , .{
            config.vus,                                 h.total,            s.hold_failed,      failed_pct,
            avgMs(h),                                   h.percentile(0.50), h.percentile(0.95), h.percentile(0.99),
            @as(f64, @floatFromInt(h.max_us)) / 1000.0,
        });
        for (&s.per_endpoint, 0..) |*e, i| std.debug.print("   {s:<7} {d:>9} requests, p95 {d:.1} ms, p99 {d:.1} ms\n", .{
            @tagName(@as(Endpoint, @fromBackingInt(@intCast(i)))), e.total, e.percentile(0.95), e.percentile(0.99),
        });
        if (s.cpu_hold_start) |cpu0| {
            const now_s = elapsedS(s);
            const cpu = serverCpuS() orelse cpu0;
            std.debug.print("   server: {d:.1}% CPU over the hold, max RSS {d:.1} MB\n", .{
                100.0 * (cpu - cpu0) / (now_s - s.hold_start_s),
                @as(f64, @floatFromInt(s.max_rss_kb)) / 1024.0,
            });
        }
    }
};

fn avgMs(h: *const Histogram) f64 {
    if (h.total == 0) return 0;
    return @as(f64, @floatFromInt(h.sum_us)) / @as(f64, @floatFromInt(h.total)) / 1000.0;
}

fn elapsedS(s: *const Stats) f64 {
    const now_ns = std.Io.Timestamp.now(UringIo.ioFromInstance(), .awake).nanoseconds;
    return @as(f64, @floatFromInt(now_ns - s.start_ns)) / std.time.ns_per_s;
}

fn report(io: Io) void {
    var last_cpu: ?f64 = serverCpuS();
    var last_t: f64 = 0;
    std.debug.print("{s:>6} {s:>7} {s:>7} {s:>8} {s:>6} {s:>8} {s:>8} {s:>8} {s:>9} {s:>6} {s:>9}\n", .{
        "t", "vus", "conns", "req/s", "fail", "p50ms", "p99ms", "maxms", "srv_cpu%", "rssMB", "tcpmemMB",
    });
    while (true) {
        io.sleep(.fromSeconds(config.interval_s), .awake) catch {};
        const t = elapsedS(&stats);
        const cpu = serverCpuS();
        const rss = serverRssKb();
        stats.max_rss_kb = @max(stats.max_rss_kb, rss);
        if (stats.vus == config.vus and stats.cpu_hold_start == null) {
            stats.cpu_hold_start = cpu;
            stats.hold_start_s = t;
        }
        const cpu_pct = if (cpu != null and last_cpu != null) 100.0 * (cpu.? - last_cpu.?) / (t - last_t) else 0;
        const h = &stats.interval;
        std.debug.print("{d:>6.0} {d:>7} {d:>7} {d:>8.0} {d:>6} {d:>8.2} {d:>8.2} {d:>8.1} {d:>9.1} {d:>6.1} {d:>9.1}\n", .{
            t,                                               stats.vus,                                  stats.connections,
            @as(f64, @floatFromInt(h.total)) / (t - last_t), stats.interval_failed,                      h.percentile(0.5),
            h.percentile(0.99),                              @as(f64, @floatFromInt(h.max_us)) / 1000.0, cpu_pct,
            @as(f64, @floatFromInt(rss)) / 1024.0,           tcpMemMb(),
        });
        stats.interval = .{};
        stats.interval_failed = 0;
        last_cpu = cpu;
        last_t = t;
    }
}

/// utime + stime of the server, in seconds.
fn serverCpuS() ?f64 {
    const pid = config.server_pid orelse return null;
    var path_buf: [64]u8 = undefined;
    var buf: [1024]u8 = undefined;
    const text = readProc(std.fmt.bufPrintSentinel(&path_buf, "/proc/{d}/stat", .{pid}, 0) catch unreachable, &buf) orelse return null;
    const after = text[(std.mem.lastIndexOfScalar(u8, text, ')') orelse return null) + 2 ..];
    var it = std.mem.tokenizeScalar(u8, after, ' ');
    var i: usize = 0;
    var ticks: u64 = 0;
    // Fields 14 and 15 (utime, stime); `after` starts at field 3.
    while (it.next()) |tok| : (i += 1) {
        if (i == 11 or i == 12) ticks += std.fmt.parseUnsigned(u64, tok, 10) catch 0;
        if (i == 12) break;
    }
    return @as(f64, @floatFromInt(ticks)) / 100.0;
}

fn serverRssKb() u64 {
    const pid = config.server_pid orelse return 0;
    var path_buf: [64]u8 = undefined;
    var buf: [4096]u8 = undefined;
    const text = readProc(std.fmt.bufPrintSentinel(&path_buf, "/proc/{d}/status", .{pid}, 0) catch unreachable, &buf) orelse return 0;
    const at = std.mem.indexOf(u8, text, "VmRSS:") orelse return 0;
    var it = std.mem.tokenizeAny(u8, text[at + 6 ..], " \t\n");
    return std.fmt.parseUnsigned(u64, it.next() orelse return 0, 10) catch 0;
}

/// Kernel memory of all TCP sockets (both ends, on loopback).
fn tcpMemMb() f64 {
    var buf: [4096]u8 = undefined;
    const text = readProc("/proc/net/sockstat", &buf) orelse return 0;
    const at = std.mem.indexOf(u8, text, " mem ") orelse return 0;
    var it = std.mem.tokenizeAny(u8, text[at + 5 ..], " \n");
    const pages = std.fmt.parseUnsigned(u64, it.next() orelse return 0, 10) catch return 0;
    return @as(f64, @floatFromInt(pages * 4096)) / (1024 * 1024);
}

fn readProc(path: [:0]const u8, buf: []u8) ?[]const u8 {
    const linux = std.os.linux;
    const rc = linux.open(path, .{ .ACCMODE = .RDONLY, .CLOEXEC = true }, 0);
    if (linux.errno(rc) != .SUCCESS) return null;
    const fd: linux.fd_t = @intCast(rc);
    defer _ = linux.close(fd);
    const n = linux.read(fd, buf.ptr, buf.len);
    if (linux.errno(n) != .SUCCESS) return null;
    return buf[0..n];
}

fn fatal(comptime fmt: []const u8, args: anytype) noreturn {
    std.log.err(fmt, args);
    std.process.exit(1);
}

const std = @import("std");
const Io = std.Io;
const net = Io.net;
const UringIo = @import("UringIo.zig");
