// Feed API: one io_uring event loop, SQLite in-process, group commit per completion batch.
// Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT. See README.md for the design.
//
// Zig 0.17 port. What changed from the 0.15.2 version, and why:
//  * @cImport is gone (0.17): the dozen SQLite entry points are declared below as plain `extern "c"`
//    functions in `c`; link the amalgamation's object file as before. No translate-c package needed.
//  * std.os.linux.IoUring was reworked (0.16/0.17). This file talks to io_uring through libc's syscall()
//    and mmap() with the kernel ABI written out in the "io_uring ABI" section, so it does not depend on
//    the std layout. Same goes for env (getenv), sockets, rlimit, time and sleep: std.posix, std.net,
//    std.time and the global argv/environ all moved or went away in 0.16.
//  * `**` array repetition is gone (0.17): @splat.  std.mem.indexOf* became find* (0.16): libc memchr/memmem.
//  * main takes std.process.Init.Minimal (0.16 "juicy main") for argv.
// Behaviour changes on top of the original design (all behind the same rules: no caching, every response
// waits for its batch's COMMIT):
//  * bounded reads after a burst instead of re-arming every ended multishot receive at once
//  * 8 SO_REUSEPORT listeners, 512 accepts in flight
//  * recycled send buffers, nice(19) checkpointer, shorter batching wait
// Merged with the B-tree read layer (src/sqlite_bt.c): /feed and /posts/:id read through SQLite's
// own B-tree cursors inside the batch transaction instead of the SQL layer, with the SQL plans kept
// as fallback; SQLite built -O3 -march=native with PGO; mmap off, 32 MB page cache. Two type errors
// in the port as pasted are fixed (bind_text pointer, ring flags cast). See README.md.
const std = @import("std");

const MAXREQ = 64 * 1024; // header + body cap per request
const BATCH_OUT = 4 << 20; // response bytes held per batch until its commit
const MAXSEG = 65536; // responses per batch
const NBUF = 4096; // provided receive buffers per ring
const BSZ = 1024;
const MAXRINGS = 32;
const NL = 8; // SO_REUSEPORT listeners: each listen queue is capped at somaxconn (4096)
const POOL_CAP = 8192; // send buffers of this size are recycled instead of malloc/free per response
const BATCH_WAIT_NS = 2_000_000; // busy-loop wait for more completions (A/B this: 0, 1, 2, 6 ms)
const BATCH_MAX = 128;
const gpa = std.heap.c_allocator;

// ---------- libc ----------
const Rlimit = extern struct { cur: u64, max: u64 };
const SockaddrIn = extern struct { family: u16, port: u16, addr: u32, zero: [8]u8 };
extern "c" fn signal(sig: c_int, handler: usize) usize;
extern "c" fn fork() c_int;
extern "c" fn execv(path: [*:0]const u8, argv: [*:null]const ?[*:0]const u8) c_int;
extern "c" fn nice(inc: c_int) c_int;
extern "c" fn _exit(code: c_int) noreturn;
extern "c" fn getenv(name: [*:0]const u8) ?[*:0]const u8;
extern "c" fn time(t: ?*i64) i64;
extern "c" fn usleep(us: c_uint) c_int;
extern "c" fn prctl(option: c_int, ...) c_int;
extern "c" fn syscall(number: c_long, ...) c_long;
extern "c" fn __errno_location() *c_int;
extern "c" fn mmap(addr: ?*anyopaque, len: usize, prot: c_int, flags: c_int, fd: c_int, off: i64) usize;
extern "c" fn madvise(addr: ?*anyopaque, len: usize, advice: c_int) c_int;
extern "c" fn eventfd(initval: c_uint, flags: c_int) c_int;
extern "c" fn socket(domain: c_int, kind: c_int, protocol: c_int) c_int;
extern "c" fn setsockopt(fd: c_int, level: c_int, name: c_int, val: *const anyopaque, len: u32) c_int;
extern "c" fn bind(fd: c_int, addr: *const anyopaque, len: u32) c_int;
extern "c" fn listen(fd: c_int, backlog: c_int) c_int;
extern "c" fn getrlimit(resource: c_int, rlim: *Rlimit) c_int;
extern "c" fn setrlimit(resource: c_int, rlim: *const Rlimit) c_int;
extern "c" fn memchr(s: [*]const u8, ch: c_int, n: usize) ?[*]const u8;
extern "c" fn memmem(h: [*]const u8, hn: usize, n: [*]const u8, nn: usize) ?[*]const u8;
extern "c" fn memmove(dst: [*]u8, src: [*]const u8, n: usize) [*]u8;

fn errno() c_int {
    return __errno_location().*;
}
fn findByte(s: []const u8, ch: u8, from: usize) ?usize { // absolute index, like indexOfScalarPos
    if (from >= s.len) return null;
    const p = memchr(s.ptr + from, ch, s.len - from) orelse return null;
    return @intFromPtr(p) - @intFromPtr(s.ptr);
}
fn findSeq(hay: []const u8, needle: []const u8) ?usize {
    if (needle.len == 0 or hay.len < needle.len) return null;
    const p = memmem(hay.ptr, hay.len, needle.ptr, needle.len) orelse return null;
    return @intFromPtr(p) - @intFromPtr(hay.ptr);
}
fn parseIp4(s: []const u8) ?u32 { // the four octets in memory order, as sockaddr_in wants them
    var out: [4]u8 = undefined;
    var n: usize = 0;
    var cur: u32 = 0;
    var digits: usize = 0;
    for (s) |ch| {
        if (ch == '.') {
            if (digits == 0 or n >= 3) return null;
            out[n] = @intCast(cur);
            n += 1;
            cur = 0;
            digits = 0;
        } else if (ch >= '0' and ch <= '9') {
            cur = cur * 10 + (ch - '0');
            digits += 1;
            if (cur > 255) return null;
        } else return null;
    }
    if (digits == 0 or n != 3) return null;
    out[3] = @intCast(cur);
    return std.mem.readInt(u32, &out, .little);
}

// ---------- SQLite (declared by hand: @cImport no longer exists) ----------
const c = struct {
    pub const sqlite3 = opaque {};
    pub const sqlite3_stmt = opaque {};
    pub const SQLITE_OK = 0;
    pub const SQLITE_ROW = 100;
    pub const SQLITE_DONE = 101;
    pub const SQLITE_OPEN_READWRITE = 2;
    pub const SQLITE_PREPARE_PERSISTENT = 1;
    pub const SQLITE_CHECKPOINT_PASSIVE = 0;
    pub extern "c" fn sqlite3_initialize() c_int;
    pub extern "c" fn sqlite3_open_v2(filename: [*:0]const u8, ppDb: *?*sqlite3, flags: c_int, zVfs: ?[*:0]const u8) c_int;
    pub extern "c" fn sqlite3_busy_timeout(db: ?*sqlite3, ms: c_int) c_int;
    pub extern "c" fn sqlite3_exec(db: ?*sqlite3, sql: [*:0]const u8, cb: ?*const anyopaque, arg: ?*anyopaque, errmsg: ?*?[*:0]u8) c_int;
    pub extern "c" fn sqlite3_errmsg(db: ?*sqlite3) [*:0]const u8;
    pub extern "c" fn sqlite3_prepare_v3(db: ?*sqlite3, sql: [*]const u8, nbyte: c_int, flags: c_uint, ppStmt: *?*sqlite3_stmt, tail: ?*?[*]const u8) c_int;
    pub extern "c" fn sqlite3_step(s: ?*sqlite3_stmt) c_int;
    pub extern "c" fn sqlite3_reset(s: ?*sqlite3_stmt) c_int;
    pub extern "c" fn sqlite3_get_autocommit(db: ?*sqlite3) c_int;
    pub extern "c" fn sqlite3_changes(db: ?*sqlite3) c_int;
    pub extern "c" fn sqlite3_column_text(s: ?*sqlite3_stmt, i: c_int) ?[*]const u8;
    pub extern "c" fn sqlite3_column_bytes(s: ?*sqlite3_stmt, i: c_int) c_int;
    pub extern "c" fn sqlite3_column_int64(s: ?*sqlite3_stmt, i: c_int) i64;
    pub extern "c" fn sqlite3_bind_int64(s: ?*sqlite3_stmt, i: c_int, v: i64) c_int;
    pub extern "c" fn sqlite3_bind_text(s: ?*sqlite3_stmt, i: c_int, text: [*]const u8, n: c_int, dtor: ?*const anyopaque) c_int;
    pub extern "c" fn sqlite3_wal_checkpoint_v2(db: ?*sqlite3, zdb: ?[*:0]const u8, mode: c_int, pnlog: *c_int, pnckpt: *c_int) c_int;
    // B-tree read layer (src/sqlite_bt.c, which #includes the amalgamation in the same TU).
    pub const bt_post = extern struct {
        id: i64,
        likes: i64,
        body: [*]const u8,
        ca: [*]const u8,
        au: [*]const u8,
        bl: c_int,
        cl: c_int,
        al: c_int,
    };
    pub extern "c" fn bt_init(db: ?*sqlite3) c_int;
    pub extern "c" fn bt_begin() c_int;
    pub extern "c" fn bt_end() void;
    pub extern "c" fn bt_feed(out: [*]bt_post, n: *c_int, buf: [*]u8, cap: usize) c_int;
    pub extern "c" fn bt_post_get(id: i64, out: *bt_post, buf: [*]u8, cap: usize) c_int;
    pub extern "c" fn bt_note_insert(id: i64, ca: [*]const u8, ca_len: c_int) void;
};

// ---------- io_uring ABI (linux/io_uring.h; constants verified against the header) ----------
const OP_ACCEPT = 13;
const OP_ASYNC_CANCEL = 14;
const OP_CLOSE = 19;
const OP_READ = 22;
const OP_SEND = 26;
const OP_RECV = 27;
const SQE_FIXED_FILE = 1 << 0;
const SQE_IO_HARDLINK = 1 << 3;
const SQE_BUFFER_SELECT = 1 << 5;
const RECV_MULTISHOT = 1 << 1;
const FILE_INDEX_ALLOC: u32 = 0xFFFFFFFF;
const SETUP_CQSIZE = 1 << 3;
const SETUP_SUBMIT_ALL = 1 << 7;
const SETUP_SINGLE_ISSUER = 1 << 12;
const SETUP_DEFER_TASKRUN = 1 << 13;
const ENTER_GETEVENTS = 1 << 0;
const ENTER_EXT_ARG = 1 << 3;
const SQ_CQ_OVERFLOW = 1 << 1;
const SQ_TASKRUN = 1 << 2;
const REG_EVENTFD = 4;
const REG_FILES2 = 13;
const REG_PBUF_RING = 22;
const RSRC_SPARSE = 1 << 0;
const CQE_F_BUFFER = 1 << 0;
const CQE_F_MORE = 1 << 1;
const CQE_BUFFER_SHIFT = 16;
const OFF_SQES = 0x10000000;
const SYS_io_uring_setup = 425;
const SYS_io_uring_enter = 426;
const SYS_io_uring_register = 427;
const SYS_close = 3;
const MSG_DONTWAIT = 0x40;
const MSG_NOSIGNAL = 0x4000;
const ENOBUFS_RES: i32 = -105;
const EAGAIN_RES: i32 = -11;

const Sqe = extern struct {
    opcode: u8 = 0,
    flags: u8 = 0,
    ioprio: u16 = 0,
    fd: i32 = 0,
    off: u64 = 0,
    addr: u64 = 0,
    len: u32 = 0,
    op_flags: u32 = 0,
    user_data: u64 = 0,
    buf_index: u16 = 0,
    personality: u16 = 0,
    file_index: u32 = 0,
    addr3: u64 = 0,
    pad: u64 = 0,
};
const Cqe = extern struct { user_data: u64, res: i32, flags: u32 };
const BufEnt = extern struct { addr: u64, len: u32, bid: u16, resv: u16 }; // entry 0's resv is the ring's tail
const BufReg = extern struct { ring_addr: u64, ring_entries: u32, bgid: u16, flags: u16, resv: [3]u64 };
const RsrcReg = extern struct { nr: u32, flags: u32, resv2: u64, data: u64, tags: u64 };
const GetArg = extern struct { sigmask: u64, sigmask_sz: u32, min_wait: u32, ts: u64 };
const Kts = extern struct { sec: i64, nsec: i64 };
const SqOff = extern struct { head: u32, tail: u32, ring_mask: u32, ring_entries: u32, flags: u32, dropped: u32, array: u32, resv1: u32, user_addr: u64 };
const CqOff = extern struct { head: u32, tail: u32, ring_mask: u32, ring_entries: u32, overflow: u32, cqes: u32, flags: u32, resv1: u32, user_addr: u64 };
const Params = extern struct {
    sq_entries: u32,
    cq_entries: u32,
    flags: u32,
    sq_thread_cpu: u32,
    sq_thread_idle: u32,
    features: u32,
    wq_fd: u32,
    resv: [3]u32,
    sq_off: SqOff,
    cq_off: CqOff,
};
comptime {
    std.debug.assert(@sizeOf(Sqe) == 64 and @sizeOf(Cqe) == 16 and @sizeOf(BufEnt) == 16);
    std.debug.assert(@sizeOf(Params) == 120 and @sizeOf(BufReg) == 40 and @sizeOf(RsrcReg) == 32 and @sizeOf(GetArg) == 24);
}

const Uring = struct {
    fd: i32 = -1,
    sq_head: *u32 = undefined,
    sq_tail: *u32 = undefined,
    sq_flags: *u32 = undefined,
    sq_mask: u32 = 0,
    sq_entries: u32 = 0,
    sq_local: u32 = 0, // tail as prepared; published to the kernel by flush()
    sqes: [*]Sqe = undefined,
    cq_head: *u32 = undefined,
    cq_tail: *u32 = undefined,
    cq_mask: u32 = 0,
    cqes: [*]Cqe = undefined,
};
fn at32(base: [*]u8, off: u32) *u32 {
    return @ptrCast(@alignCast(base + off));
}
fn uringInit(u: *Uring, entries: u32, flags: u32, cq_entries: u32) bool {
    var p = std.mem.zeroes(Params);
    p.flags = flags | SETUP_CQSIZE;
    p.cq_entries = cq_entries;
    const fd = syscall(SYS_io_uring_setup, @as(c_ulong, entries), @intFromPtr(&p));
    if (fd < 0) return false;
    u.fd = @intCast(fd);
    if (p.features & 1 == 0) return fail(u); // IORING_FEAT_SINGLE_MMAP (Linux 5.4+)
    const sq_sz: usize = p.sq_off.array + p.sq_entries * 4;
    const cq_sz: usize = p.cq_off.cqes + p.cq_entries * @sizeOf(Cqe);
    const base = mmap(null, @max(sq_sz, cq_sz), 3, 0x8001, u.fd, 0); // RW, SHARED|POPULATE
    if (base == std.math.maxInt(usize)) return fail(u);
    const sq = mmap(null, @as(usize, p.sq_entries) * @sizeOf(Sqe), 3, 0x8001, u.fd, OFF_SQES);
    if (sq == std.math.maxInt(usize)) return fail(u);
    const b: [*]u8 = @ptrFromInt(base);
    u.sq_head = at32(b, p.sq_off.head);
    u.sq_tail = at32(b, p.sq_off.tail);
    u.sq_flags = at32(b, p.sq_off.flags);
    u.sq_entries = p.sq_entries;
    u.sq_mask = p.sq_entries - 1;
    u.sq_local = u.sq_tail.*;
    u.sqes = @ptrCast(@alignCast(@as([*]u8, @ptrFromInt(sq))));
    const arr: [*]u32 = @ptrCast(@alignCast(b + p.sq_off.array)); // identity map, written once
    for (0..p.sq_entries) |i| arr[i] = @intCast(i);
    u.cq_head = at32(b, p.cq_off.head);
    u.cq_tail = at32(b, p.cq_off.tail);
    u.cq_mask = p.cq_entries - 1;
    u.cqes = @ptrCast(@alignCast(b + p.cq_off.cqes));
    return true;
}
fn fail(u: *Uring) bool {
    _ = syscall(SYS_close, @as(c_long, u.fd));
    u.fd = -1;
    return false;
}
fn uringRegister(u: *Uring, op: u32, arg: usize, n: u32) c_long {
    return syscall(SYS_io_uring_register, @as(c_long, u.fd), @as(c_ulong, op), arg, @as(c_ulong, n));
}
fn enter(u: *Uring, to_submit: u32, min_complete: u32, flags: u32, arg: usize, argsz: usize) c_long {
    return syscall(SYS_io_uring_enter, @as(c_long, u.fd), @as(c_ulong, to_submit), @as(c_ulong, min_complete), @as(c_ulong, flags), arg, argsz);
}
fn sqPending(u: *Uring) u32 { // prepared but not yet consumed by the kernel
    return u.sq_local -% @atomicLoad(u32, u.sq_head, .acquire);
}
fn flush(u: *Uring) u32 { // publish the tail; returns how many entries the kernel still has to take
    @atomicStore(u32, u.sq_tail, u.sq_local, .release);
    return sqPending(u);
}
fn submit(u: *Uring) void {
    const n = flush(u);
    if (n > 0) _ = enter(u, n, 0, 0, 0, 0);
}
fn needsFlush(u: *Uring) bool {
    return (@atomicLoad(u32, u.sq_flags, .acquire) & (SQ_CQ_OVERFLOW | SQ_TASKRUN)) != 0;
}

// A response in flight. who is the connection's receive tag, so a late completion can tell whether
// the connection it belongs to is still the same one.
const Sop = struct { who: u64, d: []u8, len: usize, off: usize = 0 };
const Conn = struct {
    in: []u8 = &.{}, // partial request, or input held while a send is in flight
    inlen: usize = 0,
    q: ?*Sop = null, // responses queued behind the send in flight
    out: ?*Sop = null, // the send in flight
    gen: u32 = 0,
    open: bool = false,
    closing: bool = false,
    sending: bool = false,
    rd: bool = false, // a single-shot bounded read (T_READ) is in flight instead of the multishot receive
};
// Accepted sockets live in each ring's registered file table, not in the process descriptor table.
// A table can't be larger than RLIMIT_NOFILE, so more rings are added as tables fill up.
const Ring = struct {
    r: Uring,
    c: []Conn,
    n: u32 = 0, // open connections
    pend: u32 = 0, // accepts in flight
    br_ents: [*]BufEnt,
    br_tail: *u16,
    br_local: u16 = 0,
    bufs: []u8,
};
var R: [MAXRINGS]Ring = undefined;
var nrings: usize = 0;
var lfds: [NL]i32 = @splat(-1);
var lpend: [NL]u32 = @splat(0); // accepts in flight per listener
var efd: i32 = -1;
var tsize: u32 = 0; // slots per ring
// Connections whose multishot receive ended (ENOBUFS, or data without MORE). They are read with
// single-shot MSG_DONTWAIT reads, a bounded number per pass, oldest first, instead of re-arming every
// multishot at once, which ran the buffer ring dry again and again past capacity.
var readq: std.ArrayList(u64) = .empty;
var sop_pool: std.ArrayList(*Sop) = .empty;
var db: ?*c.sqlite3 = null;
var bt_ok = false;
var bt_open = false;
var bt_scratch: [64 * 1024]u8 = undefined;
var st_feed: ?*c.sqlite3_stmt = null;
var st_fids: ?*c.sqlite3_stmt = null;
var st_flikes: ?*c.sqlite3_stmt = null;
var st_frows: ?*c.sqlite3_stmt = null;
var st_post: ?*c.sqlite3_stmt = null;
var st_ins: ?*c.sqlite3_stmt = null;
var st_like: ?*c.sqlite3_stmt = null;
var st_exists: ?*c.sqlite3_stmt = null;
var st_one: ?*c.sqlite3_stmt = null;
var st_begin: ?*c.sqlite3_stmt = null;
var st_commit: ?*c.sqlite3_stmt = null;
var st_rollback: ?*c.sqlite3_stmt = null;
var started: i64 = 0;
var now_s: i64 = 0;

// user_data: low 3 bits tag the operation. A send's user_data is its Sop pointer (tag 0).
const T_SEND = 0;
const T_ACCEPT = 1;
const T_RECV = 2;
const T_NOP = 3;
const T_EVFD = 4;
const T_READ = 5; // single-shot bounded read; its user_data is the receive tag with this tag swapped in
fn readUd(who: u64) u64 {
    return (who & ~@as(u64, 7)) | T_READ;
}
fn recvUd(k: usize, slot: usize, gen: u32) u64 {
    return T_RECV | @as(u64, k) << 3 | @as(u64, slot) << 8 | @as(u64, gen) << 32;
}
fn K(who: u64) usize {
    return @intCast(who >> 3 & 31);
}
fn SLOT(who: u64) u32 {
    return @intCast(who >> 8 & 0xffffff);
}

// ---------- output: one arena per batch, copied out to sends after COMMIT ----------
var arena: []u8 = undefined;
var alen: usize = 0;
const Seg = struct { who: u64, off: usize, len: usize };
var segs: []Seg = undefined;
var nsegs: usize = 0;
var deferred: std.ArrayList(u64) = .empty;
var rearm: std.ArrayList(u64) = .empty;
var arena_full = false;
var in_txn = false;
var txn_writes = false;
var batch_failed = false;

inline fn put(b: []const u8) void {
    @memcpy(arena[alen..][0..b.len], b);
    alen += b.len;
}
fn putu(v0: u64) void {
    var b: [24]u8 = undefined;
    var i: usize = 24;
    var v = v0;
    while (true) {
        i -= 1;
        b[i] = '0' + @as(u8, @intCast(v % 10));
        v /= 10;
        if (v == 0) break;
    }
    put(b[i..]);
}
const hexd = "0123456789abcdef";
const VL = std.simd.suggestVectorLength(u8) orelse 16;
fn putjs(s: []const u8) void { // JSON string contents, escaped
    const Vec = @Vector(VL, u8);
    var run: usize = 0;
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        if (s.len - i >= VL) { // VL bytes at a time up to the next one that needs escaping
            const v: Vec = s[i..][0..VL].*;
            const esc = @intFromBool(v < @as(Vec, @splat(0x20))) | @intFromBool(v == @as(Vec, @splat('"'))) |
                @intFromBool(v == @as(Vec, @splat('\\')));
            i += std.simd.firstTrue(esc != @as(@Vector(VL, u1), @splat(0))) orelse {
                i += VL - 1;
                continue;
            };
        }
        const ch = s[i];
        if (ch >= 0x20 and ch != '"' and ch != '\\') continue;
        put(s[run..i]);
        run = i + 1;
        switch (ch) {
            '"' => put("\\\""),
            '\\' => put("\\\\"),
            '\n' => put("\\n"),
            '\r' => put("\\r"),
            '\t' => put("\\t"),
            else => put(&[_]u8{ '\\', 'u', '0', '0', hexd[ch >> 4], hexd[ch & 15] }),
        }
    }
    put(s[run..]);
}
test "putjs matches a byte-at-a-time escape" {
    arena = try std.testing.allocator.alloc(u8, 1 << 16);
    defer std.testing.allocator.free(arena);
    var seed: u32 = 7;
    var s: [300]u8 = undefined;
    var want: [2000]u8 = undefined;
    for (0..20000) |_| {
        seed = seed *% 1664525 +% 1013904223;
        const n = (seed >> 8) % s.len;
        for (s[0..n]) |*x| {
            seed = seed *% 1664525 +% 1013904223;
            const r = seed >> 8;
            x.* = if (r % 8 == 0) "\"\\\n\x01\x1f\t"[(r >> 4) % 6] else @truncate(r >> 3);
        }
        var w: usize = 0;
        for (s[0..n]) |ch| {
            const e: []const u8 = switch (ch) {
                '"' => "\\\"",
                '\\' => "\\\\",
                '\n' => "\\n",
                '\r' => "\\r",
                '\t' => "\\t",
                0...8, 11, 12, 14...0x1f => &[_]u8{ '\\', 'u', '0', '0', hexd[ch >> 4], hexd[ch & 15] },
                else => &[_]u8{ch},
            };
            @memcpy(want[w..][0..e.len], e);
            w += e.len;
        }
        alen = 0;
        putjs(s[0..n]);
        try std.testing.expectEqualSlices(u8, want[0..w], arena[0..alen]);
    }
}

// response head with a blank Content-Length that finish() fills in
var body_start: usize = 0;
var cl_end: usize = 0;
fn beginResp(status: u16, close: bool) void {
    put(switch (status) {
        200 => "HTTP/1.1 200 OK\r\n",
        201 => "HTTP/1.1 201 Created\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n",
        401 => "HTTP/1.1 401 Unauthorized\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n",
        503 => "HTTP/1.1 503 Service Unavailable\r\n",
        else => "HTTP/1.1 500 Internal Server Error\r\n",
    });
    if (close) put("Connection: close\r\n");
    put("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
    cl_end = alen - 4;
    body_start = alen;
}
fn finish() void {
    var n = alen - body_start;
    var p = cl_end;
    while (true) {
        p -= 1;
        arena[p] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
        if (n == 0) break;
    }
}
fn err(status: u16, msg: []const u8, close: bool) void {
    beginResp(status, close);
    put("{\"error\":\"");
    put(msg);
    put("\"}");
    finish();
}

// ---------- SHA-256 / HMAC ----------
// A plain SHA-256 loop: without CPU SHA instructions (x86-64-v2 has none) std.crypto's SHA-256
// measured 4 to 7 times slower than this, and every token check hashes 4 to 6 blocks.
const rotr = std.math.rotr;
const SHA_K = [64]u32{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
fn shaBlock(h: *[8]u32, p: *const [64]u8) void {
    var w: [64]u32 = undefined;
    for (0..16) |i| w[i] = std.mem.readInt(u32, p[i * 4 ..][0..4], .big);
    for (16..64) |i| w[i] = w[i - 16] +% (rotr(u32, w[i - 15], 7) ^ rotr(u32, w[i - 15], 18) ^ (w[i - 15] >> 3)) +% w[i - 7] +% (rotr(u32, w[i - 2], 17) ^ rotr(u32, w[i - 2], 19) ^ (w[i - 2] >> 10));
    var v = h.*;
    for (0..64) |i| {
        const t1 = v[7] +% (rotr(u32, v[4], 6) ^ rotr(u32, v[4], 11) ^ rotr(u32, v[4], 25)) +% ((v[4] & v[5]) ^ (~v[4] & v[6])) +% SHA_K[i] +% w[i];
        const t2 = (rotr(u32, v[0], 2) ^ rotr(u32, v[0], 13) ^ rotr(u32, v[0], 22)) +% ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        const nv = [8]u32{ t1 +% t2, v[0], v[1], v[2], v[3] +% t1, v[4], v[5], v[6] }; // not in place: it reads v
        v = nv;
    }
    for (h, v) |*x, y| x.* +%= y;
}
fn shaFinish(h0: [8]u32, m: []const u8, out: *[32]u8) void { // hashes m after one 64-byte block already in h0
    var h = h0;
    var p = m;
    while (p.len >= 64) : (p = p[64..]) shaBlock(&h, p[0..64]);
    var b: [128]u8 = @splat(0);
    @memcpy(b[0..p.len], p);
    b[p.len] = 0x80;
    const n: usize = if (p.len < 56) 64 else 128;
    std.mem.writeInt(u64, b[n - 8 ..][0..8], (64 + m.len) * 8, .big);
    shaBlock(&h, b[0..64]);
    if (n == 128) shaBlock(&h, b[64..128]);
    for (h, 0..) |x, i| std.mem.writeInt(u32, out[i * 4 ..][0..4], x, .big);
}
var hm_in: [8]u32 = undefined; // HMAC key schedule: hash state after key^ipad / key^opad, computed once
var hm_out: [8]u32 = undefined;
fn hmacKey(key: []const u8) void {
    var k: [64]u8 = @splat(0);
    if (key.len > 64) std.crypto.hash.sha2.Sha256.hash(key, k[0..32], .{}) else @memcpy(k[0..key.len], key);
    const iv = [8]u32{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    var pad: [64]u8 = undefined;
    for (&pad, k) |*x, y| x.* = y ^ 0x36;
    hm_in = iv;
    shaBlock(&hm_in, &pad);
    for (&pad, k) |*x, y| x.* = y ^ 0x5c;
    hm_out = iv;
    shaBlock(&hm_out, &pad);
}
fn hmac(m: []const u8, out: *[32]u8) void {
    var ih: [32]u8 = undefined;
    shaFinish(hm_in, m, &ih);
    shaFinish(hm_out, &ih, out);
}
test "hmac matches std" {
    const H = std.crypto.auth.hmac.sha2.HmacSha256;
    var m: [300]u8 = undefined;
    for (&m, 0..) |*x, i| x.* = @truncate(i * 7);
    const k64: [64]u8 = @splat('x');
    const k100: [100]u8 = @splat('y');
    for ([_][]const u8{ "k", "twelve-dollar-challenge", &k64, &k100 }) |key| {
        hmacKey(key);
        for (0..m.len) |n| {
            var a: [32]u8 = undefined;
            var b: [32]u8 = undefined;
            hmac(m[0..n], &a);
            H.create(&b, m[0..n], key);
            try std.testing.expectEqualSlices(u8, &b, &a);
        }
    }
}

// ---------- base64url ----------
var b64t: [256]i8 = @splat(-1);
fn b64dec(s: []const u8, out: []u8) isize { // -1 on bad input
    if (s.len % 4 == 1) return -1;
    var acc: u32 = 0;
    var bits: u5 = 0;
    var o: usize = 0;
    for (s) |ch| {
        const v = b64t[ch];
        if (v < 0) return -1;
        acc = (acc << 6) | @as(u32, @intCast(v));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o] = @truncate(acc >> bits);
            o += 1;
        }
    }
    return @intCast(o);
}

// ---------- JSON (strict, RFC 8259) ----------
const Js = struct { s: []const u8, p: usize };
fn ws(j: *Js) void {
    while (j.p < j.s.len) : (j.p += 1) switch (j.s[j.p]) {
        ' ', '\t', '\n', '\r' => {},
        else => return,
    };
}
fn hex4(b: []const u8) ?u32 {
    var v: u32 = 0;
    for (b[0..4]) |ch| {
        const d: u32 = switch (ch) {
            '0'...'9' => ch - '0',
            'a'...'f' => ch - 'a' + 10,
            'A'...'F' => ch - 'A' + 10,
            else => return null,
        };
        v = v << 4 | d;
    }
    return v;
}
fn utf8Len(s: []const u8, p: usize) usize { // length of one valid UTF-8 sequence at p, 0 if invalid
    const ch = s[p];
    const left = s.len - p;
    if (ch < 0x80) return 1;
    if (ch >= 0xc2 and ch <= 0xdf) return if (left >= 2 and s[p + 1] & 0xc0 == 0x80) 2 else 0;
    if (ch >= 0xe0 and ch <= 0xef) {
        if (left < 3 or s[p + 1] & 0xc0 != 0x80 or s[p + 2] & 0xc0 != 0x80) return 0;
        if ((ch == 0xe0 and s[p + 1] < 0xa0) or (ch == 0xed and s[p + 1] > 0x9f)) return 0;
        return 3;
    }
    if (ch >= 0xf0 and ch <= 0xf4) {
        if (left < 4 or s[p + 1] & 0xc0 != 0x80 or s[p + 2] & 0xc0 != 0x80 or s[p + 3] & 0xc0 != 0x80) return 0;
        if ((ch == 0xf0 and s[p + 1] < 0x90) or (ch == 0xf4 and s[p + 1] > 0x8f)) return 0;
        return 4;
    }
    return 0;
}
// Parses the string at j.p (on its opening quote), decoding UTF-8 into out when out is non-null.
// Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit (the string is still validated).
fn jstr(j: *Js, out_in: ?[]u8) isize {
    const s = j.s;
    var out = out_in;
    var over = false;
    var p = j.p + 1;
    var o: usize = 0;
    while (p < s.len) {
        const ch = s[p];
        if (ch == '"') {
            j.p = p + 1;
            return if (over) -2 else @intCast(o);
        }
        if (ch < 0x20) return -1;
        if (ch == '\\') {
            p += 1;
            if (p >= s.len) return -1;
            const e: u8 = switch (s[p]) {
                '"' => '"',
                '\\' => '\\',
                '/' => '/',
                'b' => 8,
                'f' => 12,
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                'u' => {
                    if (s.len - p < 5) return -1;
                    var cp = hex4(s[p + 1 ..]) orelse return -1;
                    p += 4;
                    if (cp >= 0xd800 and cp <= 0xdbff and s.len - p >= 7 and s[p + 1] == '\\' and s[p + 2] == 'u') {
                        const lo = hex4(s[p + 3 ..]) orelse return -1;
                        if (lo >= 0xdc00 and lo <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            p += 6;
                        }
                    }
                    if (cp >= 0xd800 and cp <= 0xdfff) cp = 0xfffd; // lone surrogate: U+FFFD, as JS TextEncoder does
                    var u: [4]u8 = undefined;
                    const ul: usize = std.unicode.utf8Encode(@intCast(cp), &u) catch unreachable;
                    if (out) |ob| {
                        if (o + ul > ob.len) {
                            out = null;
                            over = true;
                        } else @memcpy(ob[o..][0..ul], u[0..ul]);
                    }
                    o += ul;
                    p += 1;
                    continue;
                },
                else => return -1,
            };
            if (out) |ob| {
                if (o >= ob.len) {
                    out = null;
                    over = true;
                } else ob[o] = e;
            }
            o += 1;
            p += 1;
            continue;
        }
        const l = utf8Len(s, p);
        if (l == 0) return -1;
        if (out) |ob| {
            if (o + l > ob.len) {
                out = null;
                over = true;
            } else @memcpy(ob[o..][0..l], s[p..][0..l]);
        }
        o += l;
        p += l;
    }
    return -1;
}
fn isDigit(ch: u8) bool {
    return ch >= '0' and ch <= '9';
}
fn jnumber(j: *Js) bool {
    const s = j.s;
    var p = j.p;
    if (p < s.len and s[p] == '-') p += 1;
    if (p >= s.len) return false;
    if (s[p] == '0') {
        p += 1;
    } else if (s[p] >= '1' and s[p] <= '9') {
        while (p < s.len and isDigit(s[p])) p += 1;
    } else return false;
    if (p < s.len and s[p] == '.') {
        p += 1;
        if (p >= s.len or !isDigit(s[p])) return false;
        while (p < s.len and isDigit(s[p])) p += 1;
    }
    if (p < s.len and (s[p] == 'e' or s[p] == 'E')) {
        p += 1;
        if (p < s.len and (s[p] == '+' or s[p] == '-')) p += 1;
        if (p >= s.len or !isDigit(s[p])) return false;
        while (p < s.len and isDigit(s[p])) p += 1;
    }
    j.p = p;
    return true;
}
fn jlit(j: *Js, lit: []const u8) bool {
    if (!std.mem.startsWith(u8, j.s[j.p..], lit)) return false;
    j.p += lit.len;
    return true;
}
fn jvalue(j: *Js, depth: u32) bool { // validates and skips one value
    if (depth > 64) return false;
    ws(j);
    if (j.p >= j.s.len) return false;
    switch (j.s[j.p]) {
        '"' => return jstr(j, null) >= 0,
        '{' => {
            j.p += 1;
            ws(j);
            if (j.p < j.s.len and j.s[j.p] == '}') {
                j.p += 1;
                return true;
            }
            while (true) {
                ws(j);
                if (j.p >= j.s.len or j.s[j.p] != '"' or jstr(j, null) < 0) return false;
                ws(j);
                if (j.p >= j.s.len or j.s[j.p] != ':') return false;
                j.p += 1;
                if (!jvalue(j, depth + 1)) return false;
                ws(j);
                if (j.p >= j.s.len) return false;
                if (j.s[j.p] == ',') {
                    j.p += 1;
                    continue;
                }
                if (j.s[j.p] == '}') {
                    j.p += 1;
                    return true;
                }
                return false;
            }
        },
        '[' => {
            j.p += 1;
            ws(j);
            if (j.p < j.s.len and j.s[j.p] == ']') {
                j.p += 1;
                return true;
            }
            while (true) {
                if (!jvalue(j, depth + 1)) return false;
                ws(j);
                if (j.p >= j.s.len) return false;
                if (j.s[j.p] == ',') {
                    j.p += 1;
                    continue;
                }
                if (j.s[j.p] == ']') {
                    j.p += 1;
                    return true;
                }
                return false;
            }
        },
        't' => return jlit(j, "true"),
        'f' => return jlit(j, "false"),
        'n' => return jlit(j, "null"),
        else => return jnumber(j),
    }
}
// Validates a whole document. For a top-level object it records the raw span of the last value of
// each wanted key (the last duplicate wins, as in JSON.parse). Returns false if the document is invalid.
const Want = struct { key: []const u8, found: bool = false, v: usize = 0, ve: usize = 0 };
fn jdoc(s: []const u8, w: []Want) bool {
    var j = Js{ .s = s, .p = 0 };
    for (w) |*x| x.found = false;
    ws(&j);
    if (j.p < s.len and s[j.p] == '{') {
        j.p += 1;
        ws(&j);
        if (j.p < s.len and s[j.p] == '}') {
            j.p += 1;
        } else while (true) {
            var kb: [32]u8 = undefined;
            ws(&j);
            if (j.p >= s.len or s[j.p] != '"') return false;
            const kl = jstr(&j, &kb);
            if (kl == -1) return false;
            ws(&j);
            if (j.p >= s.len or s[j.p] != ':') return false;
            j.p += 1;
            ws(&j);
            const vs = j.p;
            if (!jvalue(&j, 1)) return false;
            if (kl >= 0) for (w) |*x| {
                if (std.mem.eql(u8, kb[0..@intCast(kl)], x.key)) {
                    x.found = true;
                    x.v = vs;
                    x.ve = j.p;
                }
            };
            ws(&j);
            if (j.p >= s.len) return false;
            if (s[j.p] == ',') {
                j.p += 1;
                continue;
            }
            if (s[j.p] == '}') {
                j.p += 1;
                break;
            }
            return false;
        }
    } else if (!jvalue(&j, 0)) return false;
    ws(&j);
    return j.p == s.len;
}

// ---------- auth ----------
const Auth = struct { uid: i64 = 0, uname: [1024]u8 = undefined, unl: usize = 0 };
// Returns null and fills a on success, else the message to send with the 401.
fn auth(h: ?[]const u8, a: *Auth) ?[]const u8 {
    const bad = "invalid or expired token";
    const payload = "invalid token payload";
    const hv = h orelse return "missing bearer token";
    if (!std.mem.startsWith(u8, hv, "Bearer ")) return "missing bearer token";
    const t = hv[7..];
    const d1 = findByte(t, '.', 0) orelse return bad;
    const d2 = findByte(t, '.', d1 + 1) orelse return bad;
    if (findByte(t, '.', d2 + 1) != null) return bad;
    var sig: [48]u8 = undefined;
    var mac: [32]u8 = undefined;
    var hdr: [512]u8 = undefined;
    var pl: [2048]u8 = undefined;
    if (t.len - d2 - 1 != 43 or b64dec(t[d2 + 1 ..], &sig) != 32) return bad;
    hmac(t[0..d2], &mac);
    var diff: u8 = 0;
    for (0..32) |i| diff |= sig[i] ^ mac[i];
    if (diff != 0) return bad;
    if (d1 > 680 or d2 - d1 - 1 > 2700) return bad;
    const hn = b64dec(t[0..d1], &hdr);
    const pn = b64dec(t[d1 + 1 .. d2], &pl);
    if (hn < 0 or pn < 0) return bad;
    const hs = hdr[0..@intCast(hn)];
    var hw = [_]Want{.{ .key = "alg" }};
    if (!jdoc(hs, &hw) or !hw[0].found or hs[hw[0].v] != '"') return bad;
    var alg: [8]u8 = undefined;
    var aj = Js{ .s = hs, .p = hw[0].v };
    const al = jstr(&aj, &alg);
    if (al != 5 or !std.mem.eql(u8, alg[0..5], "HS256")) return bad;
    const ps = pl[0..@intCast(pn)];
    var pw = [_]Want{ .{ .key = "exp" }, .{ .key = "nbf" }, .{ .key = "sub" }, .{ .key = "username" } };
    if (!jdoc(ps, &pw) or !pw[0].found) return bad; // exp is required
    for (0..2) |i| if (pw[i].found) { // exp / nbf must be numbers, checked against now
        const span = ps[pw[i].v..pw[i].ve];
        if (span[0] != '-' and !isDigit(span[0])) return bad;
        const v = std.fmt.parseFloat(f64, span) catch return bad;
        const now: f64 = @floatFromInt(now_s);
        if (if (i == 0) now >= v else now < v) return bad;
    };
    if (!pw[2].found or ps[pw[2].v] != '"' or !pw[3].found or ps[pw[3].v] != '"') return payload;
    var sb: [24]u8 = undefined;
    var sj = Js{ .s = ps, .p = pw[2].v };
    const sl = jstr(&sj, &sb);
    if (sl <= 0 or sl > 15) return payload;
    var v: i64 = 0;
    for (sb[0..@intCast(sl)]) |d| {
        if (!isDigit(d)) return payload;
        v = v * 10 + (d - '0');
    }
    if (v <= 0) return payload;
    var uj = Js{ .s = ps, .p = pw[3].v };
    const ul = jstr(&uj, &a.uname);
    if (ul < 0) return payload;
    a.uid = v;
    a.unl = @intCast(ul);
    return null;
}

// ---------- database ----------
fn step(s: ?*c.sqlite3_stmt) c_int {
    return c.sqlite3_step(s);
}
fn reset(s: ?*c.sqlite3_stmt) void {
    _ = c.sqlite3_reset(s);
}
fn txn() bool { // opens the batch transaction on first use
    if (in_txn) return true;
    const rc = step(st_begin);
    reset(st_begin);
    in_txn = rc == c.SQLITE_DONE;
    return in_txn;
}
fn wstep(s: ?*c.sqlite3_stmt) c_int { // a write inside the batch transaction
    const rc = step(s);
    if (rc == c.SQLITE_ROW or rc == c.SQLITE_DONE) txn_writes = true;
    return rc;
}
fn dbFail() void { // a failed statement may have rolled the whole batch back
    if (in_txn and c.sqlite3_get_autocommit(db) != 0) {
        in_txn = false;
        if (txn_writes) batch_failed = true;
        txn_writes = false;
    }
}
fn col(s: ?*c.sqlite3_stmt, i: c_int) []const u8 {
    const p = c.sqlite3_column_text(s, i) orelse return "";
    return p[0..@intCast(c.sqlite3_column_bytes(s, i))];
}
// Opens the B-tree read cursors inside the batch transaction (after txn()). False: use SQL.
fn btBegin() bool {
    if (!bt_ok) return false;
    if (bt_open) return true;
    if (c.bt_begin() != 0) return false;
    bt_open = true;
    return true;
}
// Cursors must be closed before the batch's COMMIT/ROLLBACK (the read transaction ends there).
fn btClose() void {
    if (bt_open) {
        c.bt_end();
        bt_open = false;
    }
}
fn putBtPost(p: *const c.bt_post) void {
    put("{\"id\":");
    putu(@intCast(p.id));
    put(",\"body\":\"");
    putjs(p.body[0..@intCast(p.bl)]);
    put("\",\"created_at\":\"");
    put(p.ca[0..@intCast(p.cl)]);
    put("\",\"author\":\"");
    putjs(p.au[0..@intCast(p.al)]);
    put("\",\"like_count\":");
    putu(@intCast(p.likes));
    put("}");
}
// The feed through the B-tree cursors. 1 when the feed was written, 0 to fall back to SQL.
fn feedBt(close: bool) i32 {
    if (!btBegin()) return 0;
    var posts: [20]c.bt_post = undefined;
    var n: c_int = 0;
    if (c.bt_feed(&posts, &n, &bt_scratch, bt_scratch.len) != 0) return 0;
    beginResp(200, close);
    put("{\"posts\":[");
    for (posts[0..@intCast(n)], 0..) |*pp, i| {
        if (i > 0) put(",");
        putBtPost(pp);
    }
    put("]}");
    finish();
    return 1;
}
fn putPostRow(s: ?*c.sqlite3_stmt, likes: i64) void { // likes < 0: take column 4
    put("{\"id\":");
    putu(@intCast(c.sqlite3_column_int64(s, 0)));
    put(",\"body\":\"");
    putjs(col(s, 1));
    put("\",\"created_at\":\"");
    put(col(s, 2));
    put("\",\"author\":\"");
    putjs(col(s, 3));
    put("\",\"like_count\":");
    putu(@intCast(if (likes < 0) c.sqlite3_column_int64(s, 4) else likes));
    put("}");
}

// The feed's newest 20 ids usually sit in a narrow id range. Then one scan of that range in likes and
// one in posts replace the 20 per-row count and join lookups. Returns 1 when the feed was written,
// 0 to fall back to the reference query (ids too spread out), -1 on a database error.
var ftmp: [BATCH_OUT]u8 = undefined;
fn feedRange(close: bool) i32 {
    var ids: [20]i64 = undefined;
    var n: usize = 0;
    var rc = step(st_fids);
    while (rc == c.SQLITE_ROW) : (rc = step(st_fids)) {
        ids[n] = c.sqlite3_column_int64(st_fids, 0);
        n += 1;
    }
    reset(st_fids);
    if (rc != c.SQLITE_DONE) return -1;
    var lo: i64 = 0;
    var hi: i64 = 0;
    if (n > 0) {
        lo = ids[0];
        hi = ids[0];
        for (ids[1..n]) |id| {
            if (id < lo) lo = id;
            if (id > hi) hi = id;
        }
    }
    if (hi - lo >= 256) return 0;
    var slot: [256]i8 = @splat(-1);
    for (ids[0..n], 0..) |id, i| slot[@intCast(id - lo)] = @intCast(i);
    var likes: [20]i64 = @splat(0);
    _ = c.sqlite3_bind_int64(st_flikes, 1, lo);
    _ = c.sqlite3_bind_int64(st_flikes, 2, hi);
    rc = step(st_flikes);
    while (rc == c.SQLITE_ROW) : (rc = step(st_flikes)) {
        const k = slot[@intCast(c.sqlite3_column_int64(st_flikes, 0) - lo)];
        if (k >= 0) likes[@intCast(k)] += 1;
    }
    reset(st_flikes);
    if (rc != c.SQLITE_DONE) return -1;
    const mark = alen;
    var at: [20]usize = undefined;
    var len: [20]usize = undefined;
    var found: usize = 0;
    beginResp(200, close);
    put("{\"posts\":[");
    const rows = alen;
    _ = c.sqlite3_bind_int64(st_frows, 1, lo);
    _ = c.sqlite3_bind_int64(st_frows, 2, hi);
    rc = step(st_frows);
    while (rc == c.SQLITE_ROW) : (rc = step(st_frows)) {
        const k = slot[@intCast(c.sqlite3_column_int64(st_frows, 0) - lo)];
        if (k < 0) continue;
        const ki: usize = @intCast(k);
        at[ki] = alen;
        putPostRow(st_frows, likes[ki]);
        len[ki] = alen - at[ki];
        found += 1;
    }
    reset(st_frows);
    if (rc != c.SQLITE_DONE or found != n) {
        alen = mark;
        return if (rc != c.SQLITE_DONE) -1 else 0;
    }
    @memcpy(ftmp[0 .. alen - rows], arena[rows..alen]); // back into feed order
    alen = rows;
    for (0..n) |i| {
        if (i > 0) put(",");
        put(ftmp[at[i] - rows ..][0..len[i]]);
    }
    put("]}");
    finish();
    return 1;
}

// ---------- request handling ----------
fn parseId(s: []const u8) i64 { // positive decimal integer, else -1 (huge ids map to max: not found)
    if (s.len == 0) return -1;
    var v: i64 = 0;
    for (s) |d| {
        if (!isDigit(d)) return -1;
        v = if (v < 100000000000000000) v * 10 + (d - '0') else std.math.maxInt(i64);
    }
    return if (v > 0) v else -1;
}
fn jswsAt(s: []const u8, p: usize, e: usize) usize { // byte length of JS String.trim whitespace at p, or 0
    const ch = s[p];
    if (ch == ' ' or (ch >= 9 and ch <= 13)) return 1;
    if (e - p >= 2 and ch == 0xc2 and s[p + 1] == 0xa0) return 2;
    if (e - p >= 3) {
        const u = @as(u32, ch) << 16 | @as(u32, s[p + 1]) << 8 | s[p + 2];
        if (u == 0xe19a80 or (u >= 0xe28080 and u <= 0xe2808a) or u == 0xe280a8 or u == 0xe280a9 or u == 0xe280af or u == 0xe2819f or u == 0xe38080 or u == 0xefbbbf) return 3;
    }
    return 0;
}
fn jswsBefore(s: []const u8, b: usize, e: usize) usize {
    const ch = s[e - 1];
    if (ch == ' ' or (ch >= 9 and ch <= 13)) return 1;
    if (e - b >= 2 and s[e - 2] == 0xc2 and ch == 0xa0) return 2;
    if (e - b >= 3 and jswsAt(s, e - 3, e) == 3) return 3;
    return 0;
}

var txt: [MAXREQ]u8 = undefined;
fn handle(m: []const u8, path0: []const u8, authh: ?[]const u8, body: []const u8, close: bool) void {
    var path = path0;
    if (findByte(path, '?', 0)) |q| path = path[0..q];
    const get = std.mem.eql(u8, m, "GET");
    const post = std.mem.eql(u8, m, "POST");
    if (get and std.mem.eql(u8, path, "/feed")) {
        if (!txn()) return err(500, "internal server error", close);
        const fb = feedBt(close);
        if (fb > 0) return;
        const fr = feedRange(close);
        if (fr < 0) {
            dbFail();
            return err(500, "internal server error", close);
        }
        if (fr > 0) return;
        const mark = alen;
        beginResp(200, close);
        put("{\"posts\":[");
        var i: usize = 0;
        var rc = step(st_feed);
        while (rc == c.SQLITE_ROW) : (rc = step(st_feed)) {
            if (i > 0) put(",");
            i += 1;
            putPostRow(st_feed, -1);
        }
        reset(st_feed);
        if (rc != c.SQLITE_DONE) {
            dbFail();
            alen = mark;
            return err(500, "internal server error", close);
        }
        put("]}");
        return finish();
    }
    if (path.len > 7 and std.mem.startsWith(u8, path, "/posts/")) {
        const seg = path[7..];
        const sl = findByte(seg, '/', 0);
        if (get and sl == null) {
            const id = parseId(seg);
            if (id < 0) return err(400, "invalid post id", close);
            if (!txn()) return err(500, "internal server error", close);
            if (btBegin()) {
                var bp: c.bt_post = undefined;
                const brc = c.bt_post_get(id, &bp, &bt_scratch, bt_scratch.len);
                if (brc == 1) {
                    beginResp(200, close);
                    put("{\"post\":");
                    putBtPost(&bp);
                    put("}");
                    return finish();
                }
                if (brc == 0) return err(404, "post not found", close);
                // brc < 0: fall through to the SQL query below
            }
            _ = c.sqlite3_bind_int64(st_post, 1, id);
            const mark = alen;
            const rc = step(st_post);
            if (rc == c.SQLITE_ROW) {
                beginResp(200, close);
                put("{\"post\":");
                putPostRow(st_post, -1);
                put("}");
                finish();
            } else if (rc == c.SQLITE_DONE) {
                err(404, "post not found", close);
            } else {
                dbFail();
                alen = mark;
                err(500, "internal server error", close);
            }
            return reset(st_post);
        }
        if (post and sl != null and std.mem.eql(u8, seg[sl.?..], "/like")) {
            var a: Auth = .{};
            if (auth(authh, &a)) |e| return err(401, e, close);
            const id = parseId(seg[0..sl.?]);
            if (id < 0) return err(400, "invalid post id", close);
            if (!txn()) return err(500, "internal server error", close);
            _ = c.sqlite3_bind_int64(st_like, 1, a.uid);
            _ = c.sqlite3_bind_int64(st_like, 2, id);
            var rc = wstep(st_like);
            reset(st_like);
            if (rc != c.SQLITE_DONE) {
                dbFail();
                return err(500, "internal server error", close);
            }
            const created = c.sqlite3_changes(db) > 0;
            if (!created) {
                _ = c.sqlite3_bind_int64(st_exists, 1, id);
                rc = step(st_exists);
                reset(st_exists);
                if (rc == c.SQLITE_DONE) return err(404, "post not found", close);
                if (rc != c.SQLITE_ROW) {
                    dbFail();
                    return err(500, "internal server error", close);
                }
            }
            beginResp(if (created) 201 else 200, close);
            put("{\"liked\":true,\"already_liked\":");
            put(if (created) "false" else "true");
            put(",\"post_id\":");
            putu(@intCast(id));
            put("}");
            return finish();
        }
    }
    if (post and std.mem.eql(u8, path, "/posts")) {
        var a: Auth = .{};
        if (auth(authh, &a)) |e| return err(401, e, close);
        var w = [_]Want{.{ .key = "body" }};
        if (!jdoc(body, &w)) return err(400, "malformed JSON body", close);
        if (!w[0].found or body[w[0].v] != '"') return err(400, "body is required", close);
        var bj = Js{ .s = body, .p = w[0].v };
        const n = jstr(&bj, &txt);
        if (n < 0) return err(400, "malformed JSON body", close);
        var s: usize = 0;
        var se: usize = @intCast(n);
        while (s < se) {
            const l = jswsAt(&txt, s, se);
            if (l == 0) break;
            s += l;
        }
        while (se > s) {
            const l = jswsBefore(&txt, s, se);
            if (l == 0) break;
            se -= l;
        }
        if (s == se) return err(400, "body is required", close);
        var chars: usize = 0;
        for (txt[s..se]) |ch| chars += @intFromBool(ch & 0xc0 != 0x80);
        if (chars > 500) return err(400, "body must be at most 500 characters", close);
        if (!txn()) return err(500, "internal server error", close);
        _ = c.sqlite3_bind_int64(st_ins, 1, a.uid);
        _ = c.sqlite3_bind_text(st_ins, 2, txt[s..].ptr, @intCast(se - s), null);
        var rc = wstep(st_ins);
        if (rc != c.SQLITE_ROW) {
            reset(st_ins);
            dbFail();
            return err(500, "internal server error", close);
        }
        const nid = c.sqlite3_column_int64(st_ins, 0);
        const nca = col(st_ins, 1);
        const mark = alen;
        beginResp(201, close);
        put("{\"post\":{\"id\":");
        putu(@intCast(nid));
        put(",\"body\":\"");
        putjs(txt[s..se]);
        put("\",\"created_at\":\"");
        put(nca);
        put("\",\"author\":\"");
        putjs(a.uname[0..a.unl]);
        put("\",\"like_count\":0}}");
        finish();
        rc = step(st_ins); // run RETURNING to completion so the insert finishes
        reset(st_ins);
        if (rc != c.SQLITE_DONE) {
            dbFail();
            alen = mark;
            err(500, "internal server error", close);
        }
        if (rc == c.SQLITE_DONE and nca.len <= 32) c.bt_note_insert(nid, nca.ptr, @intCast(nca.len));
        return;
    }
    if (get and std.mem.eql(u8, path, "/health")) {
        const rc = step(st_one);
        reset(st_one);
        if (rc == c.SQLITE_ROW) {
            beginResp(200, close);
            put("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            putu(@intCast(now_s - started));
            put("}");
        } else {
            beginResp(503, close);
            put("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
            putjs(std.mem.span(c.sqlite3_errmsg(db)));
            put("\"}");
        }
        return finish();
    }
    err(404, "not found", close);
}

// Serves every complete request in buf. Returns bytes consumed, or -1 to drop the connection.
fn serve(buf: []u8, closing: *bool) isize {
    const eq = std.ascii.eqlIgnoreCase;
    var used: usize = 0;
    while (used < buf.len and !closing.*) {
        if (alen + 2 * MAXREQ > BATCH_OUT or nsegs == MAXSEG) { // the rest waits for the next batch
            arena_full = true;
            break;
        }
        const r = buf[used..];
        const he = findSeq(r, "\r\n\r\n") orelse {
            if (r.len > MAXREQ) return -1;
            break;
        };
        const hdrs = r[0 .. he + 2];
        const le = findByte(hdrs, '\r', 0).?;
        const line = r[0..le];
        const sp1 = findByte(line, ' ', 0) orelse return -1;
        const sp2 = findByte(line, ' ', sp1 + 1) orelse return -1;
        var close = std.mem.eql(u8, line[sp2 + 1 ..], "HTTP/1.0");
        var authh: ?[]const u8 = null;
        var clen: usize = 0;
        var expect = false;
        var h = le + 2;
        while (h < hdrs.len) {
            const hl = findByte(hdrs, '\r', h).?;
            const hline = r[h..hl];
            h = hl + 2;
            const ci = findByte(hline, ':', 0) orelse continue;
            const name = hline[0..ci];
            const v = std.mem.trim(u8, hline[ci + 1 ..], " \t");
            if (eq(name, "content-length")) {
                clen = 0;
                for (v) |d| {
                    if (!isDigit(d)) return -1;
                    clen = clen * 10 + (d - '0');
                    if (clen > MAXREQ) return -1;
                }
            } else if (eq(name, "authorization")) {
                authh = v;
            } else if (eq(name, "connection")) {
                if (eq(v, "close")) close = true else if (eq(v, "keep-alive")) close = false;
            } else if (eq(name, "expect")) {
                expect = eq(v, "100-continue");
            } else if (eq(name, "transfer-encoding")) return -1; // chunked uploads aren't supported
        }
        const body = he + 4;
        if (r.len - body < clen) {
            if (expect and body == r.len) put("HTTP/1.1 100 Continue\r\n\r\n");
            break;
        }
        handle(line[0..sp1], line[sp1 + 1 .. sp2], authh, r[body .. body + clen], close);
        if (close) closing.* = true;
        used += body + clen;
    }
    return @intCast(used);
}

// ---------- connections ----------
fn connOf(who: u64) ?*Conn { // null once that connection has closed
    const cn = &R[K(who)].c[SLOT(who)];
    return if (cn.open and cn.gen == @as(u32, @truncate(who >> 32))) cn else null;
}
fn sqe(k: usize) *Sqe {
    const u = &R[k].r;
    while (true) {
        if (sqPending(u) < u.sq_entries) {
            const s = &u.sqes[u.sq_local & u.sq_mask];
            u.sq_local +%= 1;
            s.* = .{};
            return s;
        }
        submit(u); // the queue is full: let the kernel take what is there
    }
}
fn freeSop(o: *Sop) void {
    if (o.d.len == POOL_CAP and sop_pool.items.len < 512) {
        if (sop_pool.append(gpa, o)) |_| {
            return;
        } else |_| {}
    }
    gpa.free(o.d);
    gpa.destroy(o);
}
fn connClose(who: u64) void {
    const k = K(who);
    const slot = SLOT(who);
    const r = &R[k];
    const cn = &r.c[slot];
    if (cn.in.len > 0) gpa.free(cn.in);
    if (cn.q) |q| freeSop(q); // a send still in flight frees its own buffer when it completes
    const out = if (cn.sending) cn.out else null;
    const rd = cn.rd;
    cn.* = .{ .gen = cn.gen +% 1 };
    r.n -= 1;
    if (r.r.sq_entries - sqPending(&r.r) < 3) submit(&r.r); // keep the chain in one submission
    // End the pending receive (and send), then free the slot. They are cancelled by their tags, which
    // the kernel finds in a hash table; cancelling by descriptor would scan every pending operation on
    // the ring, one per open connection.
    var s = sqe(k);
    s.opcode = OP_ASYNC_CANCEL;
    s.fd = -1;
    s.addr = if (rd) readUd(who) else who;
    s.flags = SQE_IO_HARDLINK;
    s.user_data = T_NOP;
    if (out) |o| {
        s = sqe(k);
        s.opcode = OP_ASYNC_CANCEL;
        s.fd = -1;
        s.addr = @intFromPtr(o);
        s.flags = SQE_IO_HARDLINK;
        s.user_data = T_NOP;
    }
    s = sqe(k);
    s.opcode = OP_CLOSE;
    s.file_index = slot + 1; // direct descriptor slot, 1-based
    s.user_data = T_NOP;
}
fn armRecv(who: u64) void { // multishot: one completion per chunk, into a provided buffer
    const s = sqe(K(who));
    s.opcode = OP_RECV;
    s.fd = @intCast(SLOT(who));
    s.ioprio = RECV_MULTISHOT;
    s.flags = SQE_FIXED_FILE | SQE_BUFFER_SELECT;
    s.buf_index = 0;
    s.user_data = who;
}
fn armRead(who: u64) void { // one bounded read into a provided buffer; EAGAIN/short read re-arms the multishot
    const s = sqe(K(who));
    s.opcode = OP_RECV;
    s.fd = @intCast(SLOT(who));
    s.op_flags = MSG_DONTWAIT;
    s.flags = SQE_FIXED_FILE | SQE_BUFFER_SELECT;
    s.buf_index = 0;
    s.user_data = readUd(who);
    if (connOf(who)) |cn| cn.rd = true;
}
fn sendOp(o: *Sop) void {
    const s = sqe(K(o.who));
    s.opcode = OP_SEND;
    s.fd = @intCast(SLOT(o.who));
    s.addr = @intFromPtr(o.d.ptr) + o.off;
    s.len = @intCast(o.len - o.off);
    s.op_flags = MSG_NOSIGNAL;
    s.flags = SQE_FIXED_FILE;
    s.user_data = @intFromPtr(o);
}
fn newSop(who: u64, cap: usize) ?*Sop {
    if (cap <= POOL_CAP) {
        if (sop_pool.pop()) |o| {
            o.who = who;
            o.len = 0;
            o.off = 0;
            return o;
        }
    }
    const o = gpa.create(Sop) catch return null;
    o.* = .{ .who = who, .d = gpa.alloc(u8, if (cap <= POOL_CAP) POOL_CAP else cap) catch {
        gpa.destroy(o);
        return null;
    }, .len = 0 };
    return o;
}
// One send in flight per connection keeps responses in order; later ones queue behind it.
fn sendBytes(who: u64, cn: *Conn, p: []const u8) void {
    if (!cn.sending) {
        const o = newSop(who, p.len) orelse return connClose(who);
        @memcpy(o.d[0..p.len], p);
        o.len = p.len;
        cn.sending = true;
        cn.out = o;
        return sendOp(o);
    }
    if (cn.q == null) cn.q = newSop(who, p.len * 2) orelse return connClose(who);
    const q = cn.q.?;
    if (q.d.len - q.len < p.len) q.d = gpa.realloc(q.d, (q.len + p.len) * 2) catch return connClose(who);
    @memcpy(q.d[q.len..][0..p.len], p);
    q.len += p.len;
}

fn push(l: *std.ArrayList(u64), who: u64) void {
    l.append(gpa, who) catch connClose(who);
}
fn process(who: u64, cn: *Conn, buf: []u8) void {
    const start = alen;
    var closing = false;
    arena_full = false;
    const ui = serve(buf, &closing);
    if (ui < 0) {
        alen = start;
        return connClose(who);
    }
    const used: usize = @intCast(ui);
    if (alen > start) {
        segs[nsegs] = .{ .who = who, .off = start, .len = alen - start };
        nsegs += 1;
    }
    cn.closing = closing;
    const left = if (closing) 0 else buf.len - used;
    if (left > 0) {
        if (buf.ptr != cn.in.ptr) {
            cn.in = gpa.alloc(u8, left + BSZ) catch return connClose(who);
            @memcpy(cn.in[0..left], buf[used..]);
        } else if (used > 0) {
            _ = memmove(cn.in.ptr, cn.in.ptr + used, left);
        }
        cn.inlen = left;
        if (arena_full) push(&deferred, who);
    } else if (cn.in.len > 0) {
        gpa.free(cn.in);
        cn.in = &.{};
        cn.inlen = 0;
    }
    if (closing and alen == start) connClose(who);
}
fn onData(who: u64, cn: *Conn, p: []u8) void {
    if (cn.inlen == 0 and !cn.sending) return process(who, cn, p);
    if (cn.in.len - cn.inlen < p.len) cn.in = gpa.realloc(cn.in, (cn.inlen + p.len) * 2) catch return connClose(who);
    @memcpy(cn.in[cn.inlen..][0..p.len], p);
    cn.inlen += p.len;
    if (cn.inlen > 4 * MAXREQ) return connClose(who); // keeps sending but won't read its replies
    if (!cn.sending) process(who, cn, cn.in[0..cn.inlen]);
}
fn sendDone(o: *Sop, res: i32) void {
    const who = o.who;
    const cn = connOf(who);
    if (cn != null and res > 0) {
        o.off += @intCast(res);
        if (o.off < o.len) return sendOp(o); // short send: the rest
    }
    freeSop(o);
    const cc = cn orelse return;
    if (res <= 0) return connClose(who);
    if (cc.q) |q| {
        cc.q = null;
        cc.out = q;
        return sendOp(q);
    }
    cc.sending = false;
    if (cc.closing) {
        connClose(who);
    } else if (cc.inlen > 0) process(who, cc, cc.in[0..cc.inlen]); // input that arrived while sending
}

// Commits the batch, then sends every response produced in it.
fn flushBatch() void {
    btClose(); // B-tree cursors close before COMMIT ends the transaction they read in
    if (in_txn) {
        const rc = step(st_commit);
        reset(st_commit);
        if (rc != c.SQLITE_DONE) {
            _ = step(st_rollback);
            reset(st_rollback);
            if (txn_writes) batch_failed = true;
        }
        in_txn = false;
        txn_writes = false;
    }
    for (segs[0..nsegs]) |s| {
        const cn = connOf(s.who) orelse continue;
        if (batch_failed) { // the batch wasn't committed: never claim it was
            connClose(s.who);
            continue;
        }
        sendBytes(s.who, cn, arena[s.off..][0..s.len]);
    }
    nsegs = 0;
    alen = 0;
    batch_failed = false;
}

// ---------- rings ----------
var evbuf: u64 = 0;
fn armEvfd() void { // extra rings signal this eventfd; ring 0 waits on it
    const s = sqe(0);
    s.opcode = OP_READ;
    s.fd = efd;
    s.addr = @intFromPtr(&evbuf);
    s.len = 8;
    s.user_data = T_EVFD;
}
fn brAdd(r: *Ring, bid: u16, off: u16) void { // never touches resv: entry 0's resv is the ring tail
    const e = &r.br_ents[(r.br_local +% off) & (NBUF - 1)];
    e.addr = @intFromPtr(r.bufs.ptr) + @as(usize, bid) * BSZ;
    e.len = BSZ;
    e.bid = bid;
}
fn brAdvance(r: *Ring, n: u16) void {
    r.br_local +%= n;
    @atomicStore(u16, r.br_tail, r.br_local, .release);
}
fn setupBufRing(r: *Ring) bool {
    const m = mmap(null, NBUF * @sizeOf(BufEnt), 3, 0x22, -1, 0); // RW, PRIVATE|ANONYMOUS
    if (m == std.math.maxInt(usize)) return false;
    const base: [*]u8 = @ptrFromInt(m);
    var reg = std.mem.zeroes(BufReg);
    reg.ring_addr = m;
    reg.ring_entries = NBUF;
    reg.bgid = 0;
    var rc = uringRegister(&r.r, REG_PBUF_RING, @intFromPtr(&reg), 1);
    if (rc < 0) { // Ubuntu 6.8.0-136+ refuses all-zero reserved words (Launchpad #2162843)
        reg.resv[0] = 1;
        rc = uringRegister(&r.r, REG_PBUF_RING, @intFromPtr(&reg), 1);
    }
    if (rc < 0) return false;
    r.br_ents = @ptrCast(@alignCast(base));
    r.br_tail = @ptrCast(@alignCast(base + 14));
    r.br_local = 0;
    return true;
}
fn huge(p: []const u8) void {
    _ = madvise(@ptrFromInt(@intFromPtr(p.ptr)), p.len, 14); // MADV_HUGEPAGE; harmless where THP is off
}
fn ringInit(k: usize) bool {
    const r = &R[k];
    // Ring 0 is the one the loop waits on, so its completions can run in that wait (DEFER_TASKRUN).
    const flags: u32 = SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER | (if (k == 0) @as(u32, SETUP_DEFER_TASKRUN) else @as(u32, 0));
    if (!uringInit(&r.r, 4096, flags, 16384)) return false;
    var rr = RsrcReg{ .nr = tsize, .flags = RSRC_SPARSE, .resv2 = 0, .data = 0, .tags = 0 };
    if (uringRegister(&r.r, REG_FILES2, @intFromPtr(&rr), @sizeOf(RsrcReg)) < 0) return fail(&r.r);
    r.c = gpa.alloc(Conn, tsize) catch @panic("out of memory");
    @memset(r.c, .{});
    huge(std.mem.sliceAsBytes(r.c));
    r.bufs = gpa.alloc(u8, NBUF * BSZ) catch @panic("out of memory");
    huge(r.bufs);
    if (!setupBufRing(r)) return fail(&r.r);
    for (0..NBUF) |i| brAdd(r, @intCast(i), @intCast(i));
    brAdvance(r, NBUF);
    r.n = 0;
    r.pend = 0;
    if (k > 0) {
        if (efd < 0) {
            efd = eventfd(0, 0x80800); // EFD_CLOEXEC | EFD_NONBLOCK
            armEvfd();
        }
        _ = uringRegister(&r.r, REG_EVENTFD, @intFromPtr(&efd), 1);
        std.debug.print("{d} connections: added ring {d}\n", .{ k * (tsize - 16), k });
    }
    nrings = k + 1;
    return true;
}
// Accepts are single-shot, a few in flight, on the first ring with room. An accept that finds its
// table full has already taken the connection and can only drop it, so tables are never filled to the
// last slot (closes still in flight may hold a few) and a multishot accept can't be stopped in time.
const ACCEPTS = 512; // rearmed once per loop pass, so this caps accepts per pass (16 held ramps to ~1.7k/s)
var naccepts: u32 = 0;
fn armAccepts() void {
    var k: usize = 0;
    while (naccepts < ACCEPTS and k < MAXRINGS) {
        if (k == nrings and !ringInit(k)) return; // retried after the next event
        if (R[k].n + R[k].pend + 16 >= tsize) {
            k += 1;
            continue;
        }
        var li: usize = 0; // the listener with the fewest accepts in flight
        for (lpend, 0..) |v, i| {
            if (v < lpend[li]) li = i;
        }
        const s = sqe(k);
        s.opcode = OP_ACCEPT;
        s.fd = lfds[li];
        s.file_index = FILE_INDEX_ALLOC;
        s.user_data = T_ACCEPT | @as(u64, li) << 3;
        lpend[li] += 1;
        R[k].pend += 1;
        naccepts += 1;
    }
}
fn onCqe(k: usize, ud: u64, res: i32, f: u32) void {
    const r = &R[k];
    switch (ud & 7) {
        T_SEND => sendDone(@ptrFromInt(ud), res),
        T_ACCEPT => {
            r.pend -= 1;
            naccepts -= 1;
            lpend[@intCast((ud >> 3) & 7)] -= 1;
            if (res >= 0) {
                const cn = &r.c[@intCast(res)];
                cn.open = true;
                r.n += 1;
                armRecv(recvUd(k, @intCast(res), cn.gen));
            }
        },
        T_RECV, T_READ => {
            const who = (ud & ~@as(u64, 7)) | T_RECV;
            const single = (ud & 7) == T_READ;
            if (single) {
                if (connOf(who)) |cn| cn.rd = false;
            }
            if (f & CQE_F_BUFFER != 0) {
                const bid: u16 = @intCast(f >> CQE_BUFFER_SHIFT);
                const p = r.bufs[@as(usize, bid) * BSZ ..][0..BSZ];
                if (res > 0) if (connOf(who)) |cn| onData(who, cn, p[0..@intCast(res)]);
                brAdd(r, bid, 0);
                brAdvance(r, 1);
            }
            if (single) {
                if (connOf(who)) |cn| {
                    if (res == BSZ or res == ENOBUFS_RES) {
                        push(&readq, who); // a full buffer: probably more waiting
                    } else if (res > 0 or res == EAGAIN_RES) {
                        push(&rearm, who); // drained: back to the multishot receive
                    } else if (cn.sending) {
                        cn.closing = true; // EOF: finish the reply in flight first
                    } else connClose(who);
                }
            } else if (f & CQE_F_MORE == 0) {
                if (connOf(who)) |cn| {
                    if (res > 0 or res == ENOBUFS_RES) {
                        push(&readq, who); // bounded reads, not an immediate re-arm
                    } else if (cn.sending) {
                        cn.closing = true; // EOF: finish the reply in flight first
                    } else connClose(who);
                }
            }
        },
        T_EVFD => armEvfd(),
        else => {},
    }
}
fn drain(k: usize) u32 {
    const u = &R[k].r;
    const tail = @atomicLoad(u32, u.cq_tail, .acquire);
    var head = u.cq_head.*;
    const n = tail -% head;
    while (head != tail) : (head +%= 1) {
        const e = u.cqes[head & u.cq_mask];
        onCqe(k, e.user_data, e.res, e.flags);
    }
    @atomicStore(u32, u.cq_head, tail, .release);
    return n;
}

fn loop() noreturn {
    var seen: u32 = 0;
    while (true) {
        { // bounded reads for connections whose multishot receive ended, oldest first
            const take = @min(readq.items.len, NBUF / 2);
            for (readq.items[0..take]) |w| if (connOf(w) != null) armRead(w);
            const rest = readq.items.len - take;
            for (0..rest) |i| readq.items[i] = readq.items[take + i];
            readq.shrinkRetainingCapacity(rest);
        }
        for (rearm.items) |w| if (connOf(w) != null) armRecv(w);
        rearm.clearRetainingCapacity();
        armAccepts();
        for (R[1..nrings]) |*r| if (sqPending(&r.r) > 0 or needsFlush(&r.r)) {
            _ = enter(&r.r, flush(&r.r), 0, ENTER_GETEVENTS, 0, 0);
        };
        // Busy (the last pass saw 4+ completions): wait for up to BATCH_MAX or BATCH_WAIT_NS, so a batch,
        // its commit and its sends cover more requests per wake-up. The target is capped at 40% of open
        // connections so a few clients without think time never sit out the timeout.
        var want: u32 = 1;
        var ts = Kts{ .sec = 0, .nsec = BATCH_WAIT_NS };
        var arg = GetArg{ .sigmask = 0, .sigmask_sz = 0, .min_wait = 0, .ts = 0 }; // ts 0: no timeout
        if (seen >= 4 and readq.items.len == 0) { // never sit in the wait with reads queued
            var open: u32 = 0;
            for (R[0..nrings]) |*r| open += r.n;
            want = @min(BATCH_MAX, open * 2 / 5 + 1);
            arg.ts = @intFromPtr(&ts);
        }
        const rc = enter(&R[0].r, flush(&R[0].r), want, ENTER_GETEVENTS | ENTER_EXT_ARG, @intFromPtr(&arg), @sizeOf(GetArg));
        if (rc < 0) switch (errno()) {
            4, 16, 62 => {}, // EINTR, EBUSY, ETIME
            else => |e| {
                std.debug.print("io_uring_enter: errno {d}\n", .{e});
                _exit(1);
            },
        };
        now_s = time(null);
        seen = 0;
        for (0..nrings) |k| seen += drain(k);
        flushBatch();
        while (deferred.items.len > 0) { // requests held back by a full arena
            var todo = deferred;
            deferred = .empty;
            for (todo.items) |w| if (connOf(w)) |cn| if (cn.inlen > 0 and !cn.sending) process(w, cn, cn.in[0..cn.inlen]);
            todo.deinit(gpa);
            flushBatch();
        }
    }
}

fn prep(sql: [:0]const u8) ?*c.sqlite3_stmt {
    var s: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v3(db, sql.ptr, -1, c.SQLITE_PREPARE_PERSISTENT, &s, null) != c.SQLITE_OK) {
        std.debug.print("prepare: {s}: {s}\n", .{ c.sqlite3_errmsg(db), sql });
        _exit(1);
    }
    return s;
}
fn dbOpen(path: [:0]const u8) void {
    if (c.sqlite3_open_v2(path.ptr, &db, c.SQLITE_OPEN_READWRITE, null) != c.SQLITE_OK) {
        std.debug.print("open {s}: {s}\n", .{ path, c.sqlite3_errmsg(db) });
        _exit(1);
    }
    _ = c.sqlite3_busy_timeout(db, 5000);
    const pragmas = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=0; PRAGMA cache_size=-32768; PRAGMA temp_store=MEMORY;" ++
        "SELECT count(*) FROM users WHERE id = 1;";
    if (c.sqlite3_exec(db, pragmas, null, null, null) != c.SQLITE_OK) {
        std.debug.print("pragma: {s}\n", .{c.sqlite3_errmsg(db)});
        _exit(1);
    }
    // The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
    // for a long time), so the WAL stays bounded at about 256 MB.
    _ = c.sqlite3_exec(db, "PRAGMA wal_autocheckpoint=8192;", null, null, null);
    const cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
    st_feed = prep(cols ++ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(cols ++ "WHERE p.id = ?");
    st_fids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    st_flikes = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?");
    st_frows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?");
    st_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    st_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
    st_exists = prep("SELECT 1 FROM posts WHERE id = ?");
    st_one = prep("SELECT 1");
    st_begin = prep("BEGIN");
    st_commit = prep("COMMIT");
    st_rollback = prep("ROLLBACK");
    bt_ok = c.bt_init(db) == 0; // if the schema is not the expected one, reads stay on SQL
    _ = step(st_feed); // takes the lock and touches the hot pages before the first request
    reset(st_feed);
}

// Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
// the length of the fsync, so a helper process does them and the event loop never waits on the disk.
fn checkpointer(path: [:0]const u8) noreturn {
    _ = prctl(1, @as(c_ulong, 15), @as(c_ulong, 0), @as(c_ulong, 0), @as(c_ulong, 0)); // PR_SET_PDEATHSIG, SIGTERM
    _ = nice(19); // one vCPU: don't compete with the event loop at equal priority
    _ = c.sqlite3_initialize();
    if (c.sqlite3_open_v2(path.ptr, &db, c.SQLITE_OPEN_READWRITE, null) != c.SQLITE_OK) _exit(1);
    _ = c.sqlite3_busy_timeout(db, 100);
    _ = c.sqlite3_exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;", null, null, null);
    while (true) {
        var log: c_int = 0;
        var done: c_int = 0;
        _ = c.sqlite3_wal_checkpoint_v2(db, null, c.SQLITE_CHECKPOINT_PASSIVE, &log, &done);
        _ = usleep(if (log > 0 and done < log) 1000 else 10_000);
    }
}

pub fn main(init: std.process.Init.Minimal) !void {
    var args = init.args.iterate();
    _ = args.next(); // argv[0]
    const ckpt = if (args.next()) |a| std.mem.eql(u8, a, "--ckpt") else false;
    const path: [:0]const u8 = std.mem.span(getenv("SQLITE_PATH") orelse return error.MissingSqlitePath);
    const secret: []const u8 = std.mem.span(getenv("JWT_SECRET") orelse return error.MissingJwtSecret);
    const host: []const u8 = if (getenv("HOST")) |h| std.mem.span(h) else "0.0.0.0";
    const port: u16 = if (getenv("PORT")) |p| try std.fmt.parseInt(u16, std.mem.span(p), 10) else 3000;
    if (ckpt) checkpointer(path);
    _ = signal(17, 1); // SIGCHLD: SIG_IGN
    var rl: Rlimit = .{ .cur = 1024, .max = 1024 };
    _ = getrlimit(7, &rl); // RLIMIT_NOFILE
    if (rl.cur < rl.max) {
        const want = Rlimit{ .cur = rl.max, .max = rl.max };
        if (setrlimit(7, &want) == 0) rl.cur = rl.max;
    }
    tsize = @intCast(@min(rl.cur, 1 << 20)); // the kernel caps a table at RLIMIT_NOFILE
    for ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 0..) |ch, i| b64t[ch] = @intCast(i);
    hmacKey(secret);
    _ = c.sqlite3_initialize(); // built with SQLITE_OMIT_AUTOINIT
    started = time(null);
    now_s = started;
    segs = try gpa.alloc(Seg, MAXSEG);
    arena = try gpa.alloc(u8, BATCH_OUT);
    huge(arena);

    dbOpen(path);
    if (fork() == 0) {
        const argv = [_:null]?[*:0]const u8{ "server", "--ckpt" };
        _ = execv("/proc/self/exe", &argv);
        _exit(1);
    }
    const ip = parseIp4(host) orelse {
        std.debug.print("bad HOST {s}\n", .{host});
        _exit(1);
    };
    const sa = SockaddrIn{ .family = 2, .port = @byteSwap(port), .addr = ip, .zero = @splat(0) }; // AF_INET
    const one = std.mem.toBytes(@as(c_int, 1));
    for (&lfds) |*lf| { // NL sockets on one port: the kernel spreads new connections over their queues
        lf.* = socket(2, 1 | 0x80000, 0); // AF_INET, SOCK_STREAM | SOCK_CLOEXEC
        _ = setsockopt(lf.*, 1, 2, &one, one.len); // SOL_SOCKET, SO_REUSEADDR
        _ = setsockopt(lf.*, 1, 15, &one, one.len); // SOL_SOCKET, SO_REUSEPORT
        _ = setsockopt(lf.*, 6, 1, &one, one.len); // IPPROTO_TCP, TCP_NODELAY: accepted sockets inherit it
        if (lf.* < 0 or bind(lf.*, &sa, @sizeOf(SockaddrIn)) < 0 or listen(lf.*, 4096) < 0) {
            std.debug.print("bind/listen {s}:{d} failed\n", .{ host, port });
            _exit(1);
        }
    }
    if (!ringInit(0)) {
        std.debug.print("io_uring setup failed (needs Linux 6.1+ with io_uring enabled)\n", .{});
        _exit(1);
    }
    loop();
}
