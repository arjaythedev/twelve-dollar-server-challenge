//! The io_uring multi-ring event loop for the $12 server challenge.
//! Architecture:
//! - 8 io_uring rings (523,776 direct descriptor capacity) with dynamic ring spill
//! - 8 SO_REUSEPORT listeners on host:port, 512 single-shot accepts in flight across listeners
//! - Provided buffer rings (1024 x 2KB) per ring
//! - Bounded single-shot MSG_DONTWAIT reads on ENOBUFS burst recovery (eliminates ENOBUFS storm)
//! - Drain-first policy: up to 16,384 reads, at most 2,048 answers per pass (keeps TCP memory ~600 pages)
//! - Zero-yield backpressure on WAL checkpoint hold: held writes queue without CPU spin loops
//! - 100% native Zig database engine with futex-coordinated background checkpoint thread
//! - Direct io_uring_enter with 1ms timeout on ring 0 (zero epoll overhead)
const std = @import("std");
const linux = std.os.linux;
const IoUring = linux.IoUring;
const sys = @import("sys.zig");
const http = @import("http.zig");
const jwt = @import("jwt.zig");
const dbm = @import("db.zig");
const bufm = @import("buf.zig");
const Scratch = @import("scratch.zig").Scratch;

const Buf = bufm.Buf;
const gpa = bufm.gpa;
const Db = dbm.Db;
const Key = jwt.Key;

const log = std.log.scoped(.server);

pub const buf_size = 2048;
pub const n_bufs_per_ring: u16 = 1024;
pub const bgid: u16 = 1;

pub const pool_buf_size: usize = 16384;
pub const pool_count: usize = 2048;

pub const k_ring_count: usize = 8;
pub const k_listener_count: usize = 8;
pub const slots_per_ring: u32 = 65472;
pub const max_pending_size: usize = 65535;

pub const ACCEPT_DEPTH: u32 = 1024;
pub const READS_PER_PASS: usize = 16384;
pub const ANSWERS_PER_PASS: u32 = 4096;
pub const WAIT_MIN: u32 = 1;
pub const WAIT_NS: i64 = 500_000; // 0.5 ms

pub const RespPool = struct {
    mem: [*]u8 = undefined,
    free_stack: []u16 = &.{},
    top: usize = 0,

    pub fn init() !RespPool {
        const total_bytes = pool_count * pool_buf_size;
        const mem = try sys.mapAnon(total_bytes);
        sys.madviseHuge(mem);

        const stack_mem = try sys.mapAnon(pool_count * @sizeOf(u16));
        const stack: []u16 = @as([*]u16, @ptrCast(@alignCast(stack_mem.ptr)))[0..pool_count];
        for (0..pool_count) |i| {
            stack[i] = @intCast(i);
        }

        return .{
            .mem = mem.ptr,
            .free_stack = stack,
            .top = pool_count,
        };
    }

    pub fn deinit(self: *RespPool) void {
        const total_bytes = pool_count * pool_buf_size;
        sys.unmap(@alignCast(self.mem[0..total_bytes]));
        sys.unmap(@alignCast(std.mem.sliceAsBytes(self.free_stack)));
        self.* = .{};
    }

    pub inline fn acquire(self: *RespPool) ?[]u8 {
        if (self.top == 0) return null;
        self.top -= 1;
        const id = self.free_stack[self.top];
        return (self.mem + (@as(usize, id) << 14))[0..pool_buf_size];
    }

    pub inline fn release(self: *RespPool, ptr: [*]u8) void {
        const offset = @intFromPtr(ptr) - @intFromPtr(self.mem);
        const id: u16 = @intCast(offset >> 14);
        self.free_stack[self.top] = id;
        self.top += 1;
    }
};

const Tag = enum(u8) {
    accept = 1,
    recv = 2,
    send = 3,
    shut = 4,
    close = 5,
    read = 6,
};

inline fn milliTimestamp() i64 {
    var ts: linux.timespec = undefined;
    _ = linux.clock_gettime(.MONOTONIC, &ts);
    return (@as(i64, ts.sec) * 1000) + @divTrunc(ts.nsec, 1_000_000);
}

pub const gen_mask: u32 = 0x00ffffff;

inline fn ud(tag: Tag, gen: u32, idx: u32) u64 {
    return (@as(u64, @intFromEnum(tag)) << 56) | (@as(u64, gen & gen_mask) << 32) | idx;
}

pub const Conn = struct {
    gen: u32 = 0,
    flags: u16 = 0,
    ring_idx: u8 = 0,
    pend_len: u16 = 0,
    pend_buf: ?[*]u8 = null,
    out: ?[*]u8 = null,
    out_len: u16 = 0,
    out_off: u16 = 0,
    pend_cap: u16 = 0,
    reserved: u8 = 0,

    pub const FLAG_OPEN: u16 = 1 << 0;
    pub const FLAG_RECV_LIVE: u16 = 1 << 1;
    pub const FLAG_SEND_LIVE: u16 = 1 << 2;
    pub const FLAG_SHUT_LIVE: u16 = 1 << 3;
    pub const FLAG_CLOSING: u16 = 1 << 4;
    pub const FLAG_OUT_POOL: u16 = 1 << 5;
    pub const FLAG_HELD: u16 = 1 << 6;
    pub const FLAG_IN_BATCH: u16 = 1 << 7;
    pub const FLAG_QUEUED: u16 = 1 << 8;
    pub const FLAG_READY: u16 = 1 << 9;
    pub const FLAG_CLOSE_AFTER: u16 = 1 << 10;
    pub const FLAG_EOF: u16 = 1 << 11;
    pub const FLAG_READ_LIVE: u16 = 1 << 12;
    pub const FLAG_ZC_PENDING: u16 = 1 << 13;

    pub inline fn isOpen(self: *const Conn) bool {
        return (self.flags & FLAG_OPEN) != 0;
    }
    pub inline fn isRecvLive(self: *const Conn) bool {
        return (self.flags & FLAG_RECV_LIVE) != 0;
    }
    pub inline fn isSendLive(self: *const Conn) bool {
        return (self.flags & FLAG_SEND_LIVE) != 0;
    }
    pub inline fn isShutLive(self: *const Conn) bool {
        return (self.flags & FLAG_SHUT_LIVE) != 0;
    }
    pub inline fn isClosing(self: *const Conn) bool {
        return (self.flags & FLAG_CLOSING) != 0;
    }
    pub inline fn isOutPool(self: *const Conn) bool {
        return (self.flags & FLAG_OUT_POOL) != 0;
    }
    pub inline fn isHeld(self: *const Conn) bool {
        return (self.flags & FLAG_HELD) != 0;
    }
    pub inline fn isInBatch(self: *const Conn) bool {
        return (self.flags & FLAG_IN_BATCH) != 0;
    }
    pub inline fn isQueued(self: *const Conn) bool {
        return (self.flags & FLAG_QUEUED) != 0;
    }
    pub inline fn isReady(self: *const Conn) bool {
        return (self.flags & FLAG_READY) != 0;
    }
    pub inline fn isCloseAfter(self: *const Conn) bool {
        return (self.flags & FLAG_CLOSE_AFTER) != 0;
    }
    pub inline fn isEof(self: *const Conn) bool {
        return (self.flags & FLAG_EOF) != 0;
    }
    pub inline fn isReadLive(self: *const Conn) bool {
        return (self.flags & FLAG_READ_LIVE) != 0;
    }
    pub inline fn isZcPending(self: *const Conn) bool {
        return (self.flags & FLAG_ZC_PENDING) != 0;
    }

    pub inline fn setOpen(self: *Conn, v: bool) void {
        self.setFlag(FLAG_OPEN, v);
    }
    pub inline fn setRecvLive(self: *Conn, v: bool) void {
        self.setFlag(FLAG_RECV_LIVE, v);
    }
    pub inline fn setSendLive(self: *Conn, v: bool) void {
        self.setFlag(FLAG_SEND_LIVE, v);
    }
    pub inline fn setShutLive(self: *Conn, v: bool) void {
        self.setFlag(FLAG_SHUT_LIVE, v);
    }
    pub inline fn setClosing(self: *Conn, v: bool) void {
        self.setFlag(FLAG_CLOSING, v);
    }
    pub inline fn setOutPool(self: *Conn, v: bool) void {
        self.setFlag(FLAG_OUT_POOL, v);
    }
    pub inline fn setHeld(self: *Conn, v: bool) void {
        self.setFlag(FLAG_HELD, v);
    }
    pub inline fn setInBatch(self: *Conn, v: bool) void {
        self.setFlag(FLAG_IN_BATCH, v);
    }
    pub inline fn setQueued(self: *Conn, v: bool) void {
        self.setFlag(FLAG_QUEUED, v);
    }
    pub inline fn setReady(self: *Conn, v: bool) void {
        self.setFlag(FLAG_READY, v);
    }
    pub inline fn setCloseAfter(self: *Conn, v: bool) void {
        self.setFlag(FLAG_CLOSE_AFTER, v);
    }
    pub inline fn setEof(self: *Conn, v: bool) void {
        self.setFlag(FLAG_EOF, v);
    }
    pub inline fn setReadLive(self: *Conn, v: bool) void {
        self.setFlag(FLAG_READ_LIVE, v);
    }
    pub inline fn setZcPending(self: *Conn, v: bool) void {
        self.setFlag(FLAG_ZC_PENDING, v);
    }

    inline fn setFlag(self: *Conn, flag: u16, v: bool) void {
        if (v) self.flags |= flag else self.flags &= ~flag;
    }
};

comptime {
    std.debug.assert(@sizeOf(Conn) == 32);
}

pub const QEntry = packed struct(u64) {
    slot: u32,
    gen: u24,
    ring_idx: u8,
};

pub const FifoQueue = struct {
    mem: [*]QEntry = undefined,
    cap: usize = 0,
    head: usize = 0,
    tail: usize = 0,

    pub fn init(cap: usize) !FifoQueue {
        const bytes = cap * @sizeOf(QEntry);
        const mem = try sys.mapAnon(bytes);
        sys.madviseHuge(mem);
        return .{
            .mem = @ptrCast(@alignCast(mem.ptr)),
            .cap = cap,
            .head = 0,
            .tail = 0,
        };
    }

    pub fn deinit(self: *FifoQueue) void {
        const bytes = self.cap * @sizeOf(QEntry);
        const raw: [*]align(4096) u8 = @ptrCast(@alignCast(self.mem));
        sys.unmap(raw[0..bytes]);
        self.* = .{};
    }

    pub inline fn push(self: *FifoQueue, item: QEntry) bool {
        const next = (self.tail + 1) & (self.cap - 1);
        if (next == self.head) return false;
        self.mem[self.tail] = item;
        self.tail = next;
        return true;
    }

    pub inline fn pop(self: *FifoQueue) ?QEntry {
        if (self.head == self.tail) return null;
        const item = self.mem[self.head];
        self.head = (self.head + 1) & (self.cap - 1);
        return item;
    }

    pub inline fn count(self: *const FifoQueue) usize {
        return (self.tail -% self.head) & (self.cap - 1);
    }

    pub inline fn isEmpty(self: *const FifoQueue) bool {
        return self.head == self.tail;
    }
};

pub const RingWorker = struct {
    id: u32 = 0,
    ring: IoUring = undefined,
    conns: []Conn = &.{},
    slots: u32 = slots_per_ring,
    nconn: u32 = 0,
    accepting: bool = false,
    accepts: [k_listener_count]u32 = @splat(0),
    full: bool = false,
    reads: u32 = 0,
    depth: u32 = ACCEPT_DEPTH,
    margin: u32 = 1024,
    pbuf_ring: [*]linux.io_uring_buf = undefined,
    pbuf_mem: [*]u8 = undefined,
    pbuf_tail: u16 = 0,
};

pub const KernelTimespec = extern struct {
    tv_sec: i64,
    tv_nsec: i64,
};

pub const IoUringGeteventsArg = extern struct {
    sigmask: u64 = 0,
    sigmask_sz: u32 = 0,
    pad: u32 = 0,
    ts: u64 = 0,
};

pub fn enterWithTimeout(fd: i32, to_submit: u32, min_complete: u32, timeout_ns: i64) usize {
    var ts = KernelTimespec{ .tv_sec = 0, .tv_nsec = timeout_ns };
    var arg = IoUringGeteventsArg{
        .sigmask = 0,
        .sigmask_sz = 0,
        .pad = 0,
        .ts = @intFromPtr(&ts),
    };
    const IORING_ENTER_GETEVENTS: u32 = 1 << 0;
    const IORING_ENTER_EXT_ARG: u32 = 1 << 3;
    const flags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
    return linux.syscall6(.io_uring_enter, @as(usize, @bitCast(@as(isize, fd))), to_submit, min_complete, flags, @intFromPtr(&arg), @sizeOf(IoUringGeteventsArg));
}

pub const Server = struct {
    rings: [k_ring_count]RingWorker = undefined,
    active_rings: usize = 0,
    listeners: [k_listener_count]i32 = @splat(-1),
    db: Db,
    jwt: Key,
    started: i64,
    now: i64,
    scratch: Scratch = undefined,
    resp_buf: Buf = .{},
    resp_pool: RespPool = undefined,

    rq: FifoQueue = undefined,
    ready: FifoQueue = undefined,
    answers_left: u32 = 0,
    need_ring: bool = false,
    spill_after: i64 = 0,

    held: [65536]QEntry = undefined,
    n_held: usize = 0,

    touched: [65536]struct { ring_idx: u8, slot: u32 } = undefined,
    n_touched: usize = 0,

    pub fn init(sqlite_path: []const u8, jwt_key: Key) !*Server {
        const smem = try sys.mapAnon(@sizeOf(Server));
        sys.madviseHuge(smem);
        const s: *Server = @ptrCast(@alignCast(smem.ptr));
        @memset(smem, 0);

        try s.db.open(sqlite_path);
        s.jwt = jwt_key;
        s.started = sys.nowUnix();
        s.now = sys.nowUnix();
        for (&s.listeners) |*l| l.* = -1;

        s.rq = try FifoQueue.init(1048576);
        s.ready = try FifoQueue.init(1048576);
        s.scratch = try Scratch.init(128 * 1024);
        s.resp_buf = try Buf.init(128 * 1024);
        s.resp_pool = try RespPool.init();

        // Pre-allocate all 8 rings so there are zero allocation / syscall pauses when spilling under load
        for (0..k_ring_count) |i| {
            try s.initWorker(@intCast(i));
        }
        s.active_rings = 1;

        return s;
    }

    pub fn deinit(self: *Server) void {
        for (0..k_ring_count) |i| {
            var rw = &self.rings[i];
            teardownPbufRing(rw);
            rw.ring.deinit();
            const cmem: []align(4096) u8 = @as([*]align(4096) u8, @ptrCast(@alignCast(rw.conns.ptr)))[0 .. @as(usize, rw.slots) * @sizeOf(Conn)];
            sys.unmap(cmem);
        }
        for (self.listeners) |fd| {
            if (fd >= 0) _ = linux.close(fd);
        }
        self.rq.deinit();
        self.ready.deinit();
        self.resp_pool.deinit();
        self.resp_buf.deinit();
        self.scratch.deinit();
        self.db.deinit();
        const smem: []align(4096) u8 = @as([*]align(4096) u8, @ptrCast(@alignCast(self)))[0..@sizeOf(Server)];
        sys.unmap(smem);
    }

    fn initWorker(self: *Server, idx: u32) !void {
        var rw = &self.rings[idx];
        rw.id = idx;
        rw.slots = slots_per_ring;
        rw.nconn = 0;
        rw.accepting = false;
        rw.accepts = @splat(0);
        rw.full = false;
        rw.reads = 0;
        rw.depth = ACCEPT_DEPTH;
        rw.margin = 1024;

        rw.ring = try initRing();
        try setupPbufRing(rw);
        try rw.ring.register_files_sparse(slots_per_ring);

        const cmem = try sys.mapAnon(@as(usize, slots_per_ring) * @sizeOf(Conn));
        sys.madviseHuge(cmem);
        rw.conns = @as([*]Conn, @ptrCast(@alignCast(cmem.ptr)))[0..slots_per_ring];
    }

    fn initRing() !IoUring {
        var last: anyerror = error.RingInitFailed;
        const sq_sizes = [_]u16{ 4096, 2048, 1024 };
        const cq_sizes = [_]u32{ 65536, 32768, 16384 };

        const IORING_SETUP_CQSIZE: u32 = 1 << 3;
        const IORING_SETUP_SUBMIT_ALL: u32 = 1 << 7;
        const IORING_SETUP_SINGLE_ISSUER: u32 = 1 << 12;
        const IORING_SETUP_DEFER_TASKRUN: u32 = 1 << 13;

        // Try high-performance single-issuer defer-taskrun flags
        for (sq_sizes) |sq| {
            for (cq_sizes) |cq| {
                var p = std.mem.zeroInit(linux.io_uring_params, .{
                    .flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL,
                });
                p.cq_entries = cq;
                if (IoUring.init_params(sq, &p)) |r| return r else |e| last = e;
            }
        }

        // Try single-issuer without defer-taskrun
        for (sq_sizes) |sq| {
            for (cq_sizes) |cq| {
                var p = std.mem.zeroInit(linux.io_uring_params, .{
                    .flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER,
                });
                p.cq_entries = cq;
                if (IoUring.init_params(sq, &p)) |r| return r else |e| last = e;
            }
        }

        // Fallback without IORING_SETUP_CQSIZE
        for (sq_sizes) |sq| {
            var p = std.mem.zeroInit(linux.io_uring_params, .{ .flags = 0 });
            if (IoUring.init_params(sq, &p)) |r| return r else |e| last = e;
        }

        return last;
    }

    fn setupPbufRing(w: *RingWorker) !void {
        const ring_bytes = @as(usize, n_bufs_per_ring) * @sizeOf(linux.io_uring_buf);
        const rmem = try sys.mapAnon(ring_bytes);
        var reg = std.mem.zeroInit(linux.io_uring_buf_reg, .{
            .ring_addr = @intFromPtr(rmem.ptr),
            .ring_entries = n_bufs_per_ring,
            .bgid = bgid,
        });
        var rc = linux.io_uring_register(w.ring.fd, .REGISTER_PBUF_RING, &reg, 1);
        if (sys.errnoOf(rc) == .INVAL) {
            reg.resv[0] = 1;
            rc = linux.io_uring_register(w.ring.fd, .REGISTER_PBUF_RING, &reg, 1);
            reg.resv[0] = 0;
        }
        if (sys.errnoOf(rc) != .SUCCESS) return error.NoBufferRings;

        const bmem = try sys.mapAnon(@as(usize, n_bufs_per_ring) * buf_size);
        sys.madviseHuge(bmem);
        w.pbuf_mem = bmem.ptr;
        w.pbuf_ring = @ptrCast(@alignCast(rmem.ptr));
        w.pbuf_tail = 0;
        for (0..n_bufs_per_ring) |i| returnBuf(w, @intCast(i));
        publishBufs(w);
    }

    fn teardownPbufRing(w: *RingWorker) void {
        var reg = std.mem.zeroInit(linux.io_uring_buf_reg, .{
            .bgid = bgid,
        });
        _ = linux.io_uring_register(w.ring.fd, .UNREGISTER_PBUF_RING, &reg, 1);
        const ring_bytes = @as(usize, n_bufs_per_ring) * @sizeOf(linux.io_uring_buf);
        const rbytes: []align(4096) u8 = @as([*]align(4096) u8, @ptrCast(@alignCast(w.pbuf_ring)))[0..ring_bytes];
        sys.unmap(rbytes);
        const bbytes: []align(4096) u8 = @as([*]align(4096) u8, @ptrCast(@alignCast(w.pbuf_mem)))[0 .. @as(usize, n_bufs_per_ring) * buf_size];
        sys.unmap(bbytes);
    }

    inline fn returnBuf(w: *RingWorker, bid: u16) void {
        const idx = w.pbuf_tail & (n_bufs_per_ring - 1);
        w.pbuf_ring[idx] = .{
            .addr = @intFromPtr(w.pbuf_mem) + (@as(usize, bid) * buf_size),
            .len = buf_size,
            .bid = bid,
            .resv = 0,
        };
        w.pbuf_tail +%= 1;
    }

    inline fn publishBufs(w: *RingWorker) void {
        const br: *linux.io_uring_buf_ring = @ptrCast(w.pbuf_ring);
        @atomicStore(u16, &br.tail, w.pbuf_tail, .release);
    }

    inline fn getSqe(w: *RingWorker) ?*linux.io_uring_sqe {
        return w.ring.get_sqe() catch null;
    }

    pub fn makeListeners(self: *Server, host: [4]u8, port: u16) !void {
        const one: c_int = 1;
        const rcv: c_int = 2048;
        const snd: c_int = 2048;
        const clamp: c_int = 2048;

        for (0..k_listener_count) |i| {
            const s = linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC, 0);
            if (sys.errnoOf(s) != .SUCCESS) return error.SocketFailed;
            const fd: i32 = @intCast(s);

            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEADDR, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEPORT, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.RCVBUF, @ptrCast(&rcv), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.SNDBUF, @ptrCast(&snd), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 10, @ptrCast(&clamp), @sizeOf(c_int)); // TCP_WINDOW_CLAMP
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 9, @ptrCast(&one), @sizeOf(c_int)); // TCP_DEFER_ACCEPT

            var sa: linux.sockaddr.in = .{ .port = std.mem.nativeToBig(u16, port), .addr = @bitCast(host) };
            if (sys.errnoOf(linux.bind(fd, @ptrCast(&sa), @sizeOf(linux.sockaddr.in))) != .SUCCESS) return error.BindFailed;
            if (sys.errnoOf(linux.listen(fd, 65535)) != .SUCCESS) return error.ListenFailed;

            self.listeners[i] = fd;
        }

        // Arm accepts on Ring 0
        self.armAccepts(&self.rings[0]);
    }

    fn armAccepts(self: *Server, w: *RingWorker) void {
        w.accepting = true;
        self.fillAccepts(w);
    }

    fn fillAccepts(self: *Server, w: *RingWorker) void {
        const target = w.depth / k_listener_count;
        for (0..k_listener_count) |li| {
            while (w.accepts[li] < target) {
                const s = getSqe(w) orelse break;
                s.prep_accept_direct(self.listeners[li], null, null, 0, linux.IORING_FILE_INDEX_ALLOC);
                s.user_data = ud(.accept, 0, @intCast(li));
                w.accepts[li] += 1;
            }
        }
    }

    fn armRecv(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        const s = getSqe(w) orelse return;
        s.prep_recv_multishot(@intCast(slot), &.{}, 0);
        s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
        s.buf_index = bgid;
        s.user_data = ud(.recv, c.gen, slot);
        c.setRecvLive(true);
    }

    fn armRead(self: *Server, w: *RingWorker, slot: u32, gen: u32) void {
        _ = self;
        const s = getSqe(w) orelse return;
        s.prep_recv(@intCast(slot), &.{}, linux.MSG.DONTWAIT);
        s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
        s.buf_index = bgid;
        s.user_data = ud(.read, gen, slot);
    }

    fn queueRead(self: *Server, ring_idx: u32, slot: u32, c: *Conn) void {
        if (!c.isQueued()) {
            c.setQueued(true);
            _ = self.rq.push(.{
                .slot = slot,
                .gen = @truncate(c.gen),
                .ring_idx = @intCast(ring_idx),
            });
        }
    }

    inline fn hasCqes(w: *const RingWorker) bool {
        const head = w.ring.cq.head.*;
        const tail = @atomicLoad(u32, w.ring.cq.tail, .acquire);
        return head != tail;
    }

    pub var running: std.atomic.Value(bool) = std.atomic.Value(bool).init(true);

    pub fn run(self: *Server) !void {
        while (running.load(.monotonic)) {
            var any_cqes = false;
            for (0..self.active_rings) |i| {
                if (hasCqes(&self.rings[i])) {
                    any_cqes = true;
                    break;
                }
            }
            const busy = (!self.rq.isEmpty() or !self.ready.isEmpty() or any_cqes);

            var accept_ri: usize = 0;
            for (0..self.active_rings) |i| {
                if (self.rings[i].accepting) {
                    accept_ri = i;
                    break;
                }
            }

            // Iterate active rings in reverse order (highest down to 0)
            var ri: usize = self.active_rings;
            while (ri > 0) {
                ri -= 1;
                const w = &self.rings[ri];
                const to_submit = w.ring.flush_sq();

                if (ri == accept_ri and !busy) {
                    _ = enterWithTimeout(w.ring.fd, to_submit, WAIT_MIN, WAIT_NS);
                } else if (to_submit > 0 or (w.ring.flags & (1 << 13) != 0)) {
                    _ = linux.io_uring_enter(w.ring.fd, to_submit, 0, linux.IORING_ENTER_GETEVENTS, null);
                }
            }

            self.answers_left = ANSWERS_PER_PASS;

            // Drain CQEs across all active rings
            for (0..self.active_rings) |i| {
                _ = self.drainRing(&self.rings[i]);
            }

            // Answer connections on ready list (oldest first, drain first)
            self.runReady();

            // Run bounded single-shot reads for connections that encountered ENOBUFS
            _ = self.readRounds();

            // Release held writes if WAL checkpoint hold has finished
            if (self.n_held > 0 and !self.db.writesHeld()) {
                self.releaseHeld();
            }

            // Commit batch and flush responses
            self.endBatch();
        }
        self.deinit();
    }

    fn drainRing(self: *Server, w: *RingWorker) usize {
        const head = w.ring.cq.head.*;
        const tail = @atomicLoad(u32, w.ring.cq.tail, .acquire);
        const count = tail -% head;
        if (count == 0) return 0;

        var returned: usize = 0;
        const mask = w.ring.cq.mask;
        var cur = head;
        while (cur != tail) : (cur +%= 1) {
            const cqe = &w.ring.cq.cqes[cur & mask];
            const tag: Tag = @enumFromInt(@as(u8, @truncate(cqe.user_data >> 56)));
            const gen: u32 = @truncate((cqe.user_data >> 32) & gen_mask);
            const slot: u32 = @truncate(cqe.user_data);

            switch (tag) {
                .accept => self.onAccept(w, cqe.res, slot),
                .recv => {
                    self.onRecv(w, slot, gen, cqe.res, cqe.flags);
                    if (cqe.flags & linux.IORING_CQE_F_BUFFER != 0) {
                        const bid: u16 = @truncate(cqe.flags >> linux.IORING_CQE_BUFFER_SHIFT);
                        returnBuf(w, bid);
                        returned += 1;
                    }
                },
                .read => {
                    w.reads -%= 1;
                    self.onRead(w, slot, gen, cqe.res, cqe.flags);
                    if (cqe.flags & linux.IORING_CQE_F_BUFFER != 0) {
                        const bid: u16 = @truncate(cqe.flags >> linux.IORING_CQE_BUFFER_SHIFT);
                        returnBuf(w, bid);
                        returned += 1;
                    }
                },
                .send => self.onSend(w, slot, gen, cqe.res, cqe.flags),
                .shut => self.onShut(w, slot, gen),
                .close => self.onClose(w, slot, gen),
            }
        }
        w.ring.cq_advance(count);

        if (returned > 0) {
            publishBufs(w);
        }

        if (w.accepting) {
            if (w.full) {
                w.accepting = false;
                self.need_ring = true;
            } else {
                self.fillAccepts(w);
            }
        }

        return count;
    }

    fn onAccept(self: *Server, w: *RingWorker, res: i32, listener_idx: u32) void {
        if (listener_idx < k_listener_count) {
            w.accepts[listener_idx] -%= 1;
        }
        if (res >= 0) {
            const slot: u32 = @intCast(res);
            if (slot >= w.slots) return;

            const c = &w.conns[slot];
            c.* = .{
                .gen = (c.gen +% 1) & gen_mask,
                .flags = Conn.FLAG_OPEN,
                .ring_idx = @intCast(w.id),
            };
            w.nconn += 1;
            self.armRecv(w, slot, c);

            if (w.nconn + w.margin >= w.slots) {
                w.full = true;
            }
        } else if (res == -@as(i32, @intCast(@intFromEnum(linux.E.NFILE))) or
            res == -@as(i32, @intCast(@intFromEnum(linux.E.MFILE))))
        {
            w.full = true;
        }
    }

    fn onRecv(self: *Server, w: *RingWorker, slot: u32, gen: u32, res: i32, flags: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (!c.isOpen() or c.gen != gen or c.isClosing()) return;

        if (res > 0) {
            const bid: u16 = @truncate(flags >> linux.IORING_CQE_BUFFER_SHIFT);
            const data = (w.pbuf_mem + (@as(usize, bid) * buf_size))[0..@intCast(res)];
            self.onDataReceived(w, slot, gen, c, data);

            if (flags & linux.IORING_CQE_F_MORE == 0) {
                c.setRecvLive(false);
                self.queueRead(w.id, slot, c);
            }
        } else if (res == -@as(i32, @intCast(@intFromEnum(linux.E.NOBUFS))) or res == -105) {
            c.setRecvLive(false);
            self.queueRead(w.id, slot, c);
        } else {
            c.setRecvLive(false);
            self.finishConn(w, slot, c);
        }
    }

    fn onRead(self: *Server, w: *RingWorker, slot: u32, gen: u32, res: i32, flags: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (!c.isOpen() or c.gen != gen or c.isClosing()) return;

        c.setReadLive(false);

        if (res > 0) {
            const bid: u16 = @truncate(flags >> linux.IORING_CQE_BUFFER_SHIFT);
            const data = (w.pbuf_mem + (@as(usize, bid) * buf_size))[0..@intCast(res)];
            self.onDataReceived(w, slot, gen, c, data);

            if (@as(usize, @intCast(res)) == buf_size) {
                self.queueRead(w.id, slot, c);
            } else {
                self.armRecv(w, slot, c);
            }
        } else if (res == -@as(i32, @intCast(@intFromEnum(linux.E.AGAIN)))) {
            self.armRecv(w, slot, c);
        } else if (res == -@as(i32, @intCast(@intFromEnum(linux.E.NOBUFS))) or res == -105) {
            self.queueRead(w.id, slot, c);
        } else {
            self.finishConn(w, slot, c);
        }
    }

    fn onDataReceived(self: *Server, w: *RingWorker, slot: u32, gen: u32, c: *Conn, data: []const u8) void {
        _ = gen;
        if (c.isCloseAfter()) return;

        if (c.isHeld() or c.isReady() or self.answers_left == 0) {
            self.stashPartial(c, data);
            if (!c.isHeld() and !c.isReady()) {
                c.setReady(true);
                _ = self.ready.push(.{
                    .slot = slot,
                    .gen = @truncate(c.gen),
                    .ring_idx = @intCast(w.id),
                });
            }
        } else {
            self.answers_left -= 1;
            self.serve(w, slot, c, data);
        }
    }

    fn runReady(self: *Server) void {
        while (self.answers_left > 0 and !self.ready.isEmpty()) {
            const ps = self.ready.pop() orelse break;
            const w = &self.rings[ps.ring_idx];
            const c = &w.conns[ps.slot];

            if (c.gen != ps.gen or !c.isOpen() or c.isClosing()) continue;

            c.setReady(false);
            if (c.isHeld()) continue;

            self.answers_left -= 1;
            self.serve(w, ps.slot, c, &.{});

            if (c.isEof() and !c.isHeld()) {
                c.setCloseAfter(true);
                self.markTouched(ps.ring_idx, ps.slot, c);
            }
        }
    }

    fn issueReads(self: *Server, budget: usize) usize {
        var issued: usize = 0;
        const count = self.rq.count();
        var idx: usize = 0;

        while (idx < count and issued < budget) : (idx += 1) {
            const ps = self.rq.pop() orelse break;
            const w = &self.rings[ps.ring_idx];
            const c = &w.conns[ps.slot];

            if (c.gen != ps.gen or !c.isOpen() or c.isClosing()) continue;

            if (c.isReadLive() or w.reads >= n_bufs_per_ring) {
                _ = self.rq.push(ps);
                continue;
            }

            c.setQueued(false);
            c.setReadLive(true);
            w.reads += 1;
            issued += 1;

            self.armRead(w, ps.slot, c.gen);
        }
        return issued;
    }

    fn readRounds(self: *Server) usize {
        var n: usize = 0;
        var budget: usize = READS_PER_PASS;

        while (budget > 0 and !self.rq.isEmpty()) {
            const k = self.issueReads(budget);
            if (k == 0) break;
            budget -= k;

            for (0..self.active_rings) |i| {
                const w = &self.rings[i];
                const to_submit = w.ring.flush_sq();
                if (to_submit > 0) {
                    _ = linux.io_uring_enter(w.ring.fd, to_submit, 0, linux.IORING_ENTER_GETEVENTS, null);
                }
            }

            for (0..self.active_rings) |i| {
                n += self.drainRing(&self.rings[i]);
            }
        }
        return n;
    }

    fn releaseHeld(self: *Server) void {
        const count = self.n_held;
        self.n_held = 0;

        for (self.held[0..count]) |he| {
            const w = &self.rings[he.ring_idx];
            const c = &w.conns[he.slot];

            if (c.gen == he.gen and c.isOpen() and !c.isClosing()) {
                c.setHeld(false);
                self.serve(w, he.slot, c, &.{});

                if (c.isEof() and !c.isHeld()) {
                    c.setCloseAfter(true);
                    self.markTouched(he.ring_idx, he.slot, c);
                }
            }
        }
    }

    fn endBatch(self: *Server) void {
        const committed = self.db.commitTx();
        self.db.tick();

        const count = self.n_touched;
        self.n_touched = 0;

        for (self.touched[0..count]) |te| {
            const w = &self.rings[te.ring_idx];
            const c = &w.conns[te.slot];

            if (!committed and c.isInBatch()) {
                if (c.out) |m| {
                    if (c.isOutPool()) self.resp_pool.release(m) else gpa.free(m[0..c.out_len]);
                    c.out = null;
                }
                c.out_len = 0;
                c.out_off = 0;
                c.setOutPool(false);
                self.sendError(w, te.slot, c, 500, "internal server error", false);
                c.setCloseAfter(true);
            }

            c.setInBatch(false);

            if (!c.isOpen() or c.isClosing() or c.isSendLive()) continue;

            if (c.out != null) {
                self.armSend(w, te.slot, c);
            } else if (c.isCloseAfter()) {
                self.armClose(w, te.slot, c);
            }
        }

        if (self.need_ring) {
            const now_ms = milliTimestamp();
            if (now_ms >= self.spill_after) {
                self.need_ring = !self.spill();
                if (self.need_ring) {
                    self.spill_after = now_ms + 1000;
                }
            }
        }
    }

    fn spill(self: *Server) bool {
        for (0..self.active_rings) |i| {
            if (self.rings[i].accepting) return true;
        }
        for (0..self.active_rings) |i| {
            if (!self.rings[i].full) {
                self.armAccepts(&self.rings[i]);
                return true;
            }
        }
        if (self.active_rings >= k_ring_count) return false;

        const next_id: u32 = @intCast(self.active_rings);
        self.active_rings += 1;
        self.armAccepts(&self.rings[next_id]);
        return true;
    }

    fn markTouched(self: *Server, ring_idx: u8, slot: u32, c: *Conn) void {
        if (!c.isInBatch()) {
            c.setInBatch(true);
            if (self.n_touched < self.touched.len) {
                self.touched[self.n_touched] = .{ .ring_idx = ring_idx, .slot = slot };
                self.n_touched += 1;
            }
        }
    }

    pub fn savePending(self: *Server, c: *Conn, remaining: []const u8) bool {
        if (remaining.len > max_pending_size) return false;
        c.pend_len = 0;
        self.stashPartial(c, remaining);
        return c.pend_len == remaining.len;
    }

    fn stashPartial(self: *Server, c: *Conn, data: []const u8) void {
        _ = self;
        if (data.len == 0) return;
        const total = @as(usize, c.pend_len) + data.len;
        if (total > max_pending_size) return;

        if (c.pend_buf == null) {
            const cap: usize = @min(max_pending_size, @max(@as(usize, 512), total));
            const mem = gpa.alloc(u8, cap) catch return;
            c.pend_buf = mem.ptr;
            c.pend_cap = @intCast(cap);
        } else if (total > c.pend_cap) {
            var new_cap: usize = @as(usize, c.pend_cap) * 2;
            while (new_cap < total) {
                if (new_cap > max_pending_size / 2) {
                    new_cap = max_pending_size;
                    break;
                }
                new_cap *= 2;
            }
            if (new_cap < total) return;
            const new_mem = gpa.alloc(u8, new_cap) catch return;
            @memcpy(new_mem[0..c.pend_len], c.pend_buf.?[0..c.pend_len]);
            gpa.free(c.pend_buf.?[0..c.pend_cap]);
            c.pend_buf = new_mem.ptr;
            c.pend_cap = @intCast(new_cap);
        }

        @memcpy((c.pend_buf.? + c.pend_len)[0..data.len], data);
        c.pend_len = @intCast(total);
    }

    fn finishConn(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        if (c.isHeld() or c.isReady()) {
            c.setEof(true);
        } else {
            c.setCloseAfter(true);
            self.markTouched(@intCast(w.id), slot, c);
        }
    }

    fn serve(self: *Server, w: *RingWorker, slot: u32, c: *Conn, data: []const u8) void {
        if (c.isCloseAfter()) return;

        var input: []const u8 = undefined;
        var using_pend = false;

        if (c.pend_len == 0) {
            if (data.len == 0) return;
            input = data;
        } else {
            if (data.len > 0) {
                self.stashPartial(c, data);
            }
            if (c.pend_len == 0) return;
            input = c.pend_buf.?[0..c.pend_len];
            using_pend = true;
        }

        var offset: usize = 0;

        while (offset < input.len and c.isOpen() and !c.isClosing() and c.out == null) {
            const rem = input[offset..];
            const parsed = http.parseHead(rem);
            switch (parsed) {
                .ok => |head| {
                    const req_len = head.head_len + head.content_length;
                    if (offset + req_len > input.len) {
                        break;
                    }
                    const body = rem[head.head_len..req_len];

                    // Check write hold during active WAL checkpoint tail
                    if ((head.method == .post or head.method == .delete) and self.db.writesHeld()) {
                        if (!c.isHeld()) {
                            c.setHeld(true);
                            if (self.n_held < self.held.len) {
                                self.held[self.n_held] = .{
                                    .slot = slot,
                                    .gen = @truncate(c.gen),
                                    .ring_idx = @intCast(w.id),
                                };
                                self.n_held += 1;
                            }
                        }
                        break;
                    }

                    offset += req_len;
                    self.dispatchRoute(w, slot, c, &head, body);
                },
                .need_more => break,
                .bad => |status| {
                    self.sendError(w, slot, c, status, "bad request", false);
                    c.setCloseAfter(true);
                    offset = input.len;
                    break;
                },
            }
        }

        const remaining = input.len - offset;
        if (remaining == 0) {
            if (using_pend) {
                if (c.pend_buf) |p| {
                    gpa.free(p[0..c.pend_cap]);
                    c.pend_buf = null;
                    c.pend_cap = 0;
                }
                c.pend_len = 0;
            }
        } else {
            if (!using_pend) {
                self.stashPartial(c, input[offset..input.len]);
            } else {
                std.mem.copyForwards(u8, c.pend_buf.?[0..remaining], c.pend_buf.?[offset..input.len]);
                c.pend_len = @intCast(remaining);
            }
        }
    }

    fn dispatchRoute(self: *Server, w: *RingWorker, slot: u32, c: *Conn, head: *const http.Head, body: []const u8) void {
        self.scratch.reset();
        self.resp_buf.reset();
        const keep_alive = head.keep_alive;
        if (!keep_alive) c.setCloseAfter(true);

        const path = head.path;
        const method = head.method;

        // Route: GET /feed
        if (method == .get and std.mem.eql(u8, path, "/feed")) {
            if (self.resp_pool.acquire()) |p| {
                var feed_buf = Buf{
                    .ptr = p.ptr + 128,
                    .len = 0,
                    .cap = pool_buf_size - 128,
                };
                if (self.db.feed(&feed_buf)) {
                    var hdr_buf: [128]u8 = undefined;
                    const hdr_len = writeHeaderDirect(&hdr_buf, 200, feed_buf.len, keep_alive);
                    const start_off = 128 - hdr_len;
                    @memcpy(p.ptr[start_off..128], hdr_buf[0..hdr_len]);

                    c.out = p.ptr + start_off;
                    c.out_len = @intCast(hdr_len + feed_buf.len);
                    c.out_off = 0;
                    c.setOutPool(true);
                    self.markTouched(@intCast(w.id), slot, c);
                    return;
                } else |_| {
                    self.resp_pool.release(p.ptr);
                    self.sendError(w, slot, c, 500, "internal server error", keep_alive);
                    return;
                }
            }
            self.db.feed(&self.resp_buf) catch {
                self.sendError(w, slot, c, 500, "internal server error", keep_alive);
                return;
            };
            self.sendResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive);
            return;
        }

        // Route: /posts/...
        if (std.mem.startsWith(u8, path, "/posts")) {
            const rem = path[6..];

            // POST /posts (create)
            if (rem.len == 0 and method == .post) {
                const claims = self.authenticate(head.auth) catch |err| {
                    self.sendAuthError(w, slot, c, err, keep_alive);
                    return;
                };

                const jp = @import("jp.zig");
                var f: [1]jp.Field = undefined;
                jp.parseObject(body, &self.scratch, &.{"body"}, &f) catch {
                    self.sendError(w, slot, c, 400, "malformed JSON body", keep_alive);
                    return;
                };

                if (f[0].kind != .string) {
                    self.sendError(w, slot, c, 400, "body is required", keep_alive);
                    return;
                }

                const trimmed = trimJs(f[0].s);
                if (trimmed.len == 0) {
                    self.sendError(w, slot, c, 400, "body is required", keep_alive);
                    return;
                }

                const utf8_len = std.unicode.utf8CountCodepoints(trimmed) catch {
                    self.sendError(w, slot, c, 400, "invalid utf-8", keep_alive);
                    return;
                };

                if (utf8_len > 500) {
                    self.sendError(w, slot, c, 400, "body must be at most 500 characters", keep_alive);
                    return;
                }

                self.db.createPost(claims.uid, claims.name, trimmed, &self.resp_buf) catch {
                    self.sendError(w, slot, c, 500, "internal server error", keep_alive);
                    return;
                };
                self.sendResponse(w, slot, c, 201, self.resp_buf.slice(), keep_alive);
                return;
            }

            // Subpaths under /posts/
            if (rem.len > 1 and rem[0] == '/') {
                const subpath = rem[1..];

                // POST /posts/:id/like or DELETE /posts/:id/like
                if (std.mem.endsWith(u8, subpath, "/like")) {
                    const id_str = subpath[0 .. subpath.len - 5];

                    const claims = self.authenticate(head.auth) catch |err| {
                        self.sendAuthError(w, slot, c, err, keep_alive);
                        return;
                    };

                    const post_id = parseId(id_str) orelse {
                        self.sendError(w, slot, c, 400, "invalid post id", keep_alive);
                        return;
                    };

                    if (method == .post) {
                        switch (self.db.likePost(claims.uid, post_id, &self.resp_buf)) {
                            .created => self.sendResponse(w, slot, c, 201, self.resp_buf.slice(), keep_alive),
                            .already_liked => self.sendResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive),
                            .not_found => self.sendError(w, slot, c, 404, "post not found", keep_alive),
                            .err => self.sendError(w, slot, c, 500, "internal server error", keep_alive),
                        }
                        return;
                    } else if (method == .delete) {
                        switch (self.db.unlikePost(claims.uid, post_id, &self.resp_buf)) {
                            .ok => self.sendResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive),
                            .not_found => self.sendError(w, slot, c, 404, "post not found", keep_alive),
                            .err => self.sendError(w, slot, c, 500, "internal server error", keep_alive),
                        }
                        return;
                    }
                }

                // Single post: GET /posts/:id
                if (method == .get) {
                    const post_id = parseId(subpath) orelse {
                        self.sendError(w, slot, c, 400, "invalid post id", keep_alive);
                        return;
                    };

                    if (self.resp_pool.acquire()) |p| {
                        const res = self.db.getPostDirect(post_id, p.ptr, keep_alive);
                        switch (res) {
                            .ok => |total_len| {
                                c.out = p.ptr;
                                c.out_len = @intCast(total_len);
                                c.out_off = 0;
                                c.setOutPool(true);
                                self.markTouched(@intCast(w.id), slot, c);
                                return;
                            },
                            .not_found => {
                                self.resp_pool.release(p.ptr);
                                self.sendError(w, slot, c, 404, "post not found", keep_alive);
                                return;
                            },
                            .err => {
                                self.resp_pool.release(p.ptr);
                                self.sendError(w, slot, c, 500, "internal server error", keep_alive);
                                return;
                            },
                        }
                    }

                    switch (self.db.getPost(post_id, &self.resp_buf)) {
                        .ok => self.sendResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive),
                        .not_found => self.sendError(w, slot, c, 404, "post not found", keep_alive),
                        .err => self.sendError(w, slot, c, 500, "internal server error", keep_alive),
                    }
                    return;
                }
            }
        }

        // Route: GET /health
        if (method == .get and std.mem.eql(u8, path, "/health")) {
            self.now = sys.nowUnix();
            const uptime = self.now - self.started;
            const ok = self.db.health(&self.resp_buf, uptime) catch false;
            self.sendResponse(w, slot, c, if (ok) 200 else 503, self.resp_buf.slice(), keep_alive);
            return;
        }

        self.sendError(w, slot, c, 404, "not found", keep_alive);
    }

    const AuthError = error{ MissingToken, InvalidToken, InvalidPayload };

    fn authenticate(self: *Server, auth_header: ?[]const u8) AuthError!jwt.Claims {
        const h = auth_header orelse return error.MissingToken;
        if (!std.mem.startsWith(u8, h, "Bearer ")) return error.MissingToken;
        const token = std.mem.trim(u8, h[7..], " \t");
        if (token.len == 0) return error.MissingToken;

        self.now = sys.nowUnix();
        switch (self.jwt.verify(token, self.now, &self.scratch)) {
            .ok => |claims| return claims,
            .invalid => return error.InvalidToken,
            .payload => return error.InvalidPayload,
        }
    }

    fn sendAuthError(self: *Server, w: *RingWorker, slot: u32, c: *Conn, err: AuthError, keep_alive: bool) void {
        switch (err) {
            error.MissingToken => self.sendError(w, slot, c, 401, "missing bearer token", keep_alive),
            error.InvalidToken => self.sendError(w, slot, c, 401, "invalid or expired token", keep_alive),
            error.InvalidPayload => self.sendError(w, slot, c, 401, "invalid token payload", keep_alive),
        }
    }

    fn sendError(self: *Server, w: *RingWorker, slot: u32, c: *Conn, status: u16, msg: []const u8, keep_alive: bool) void {
        self.resp_buf.reset();
        self.resp_buf.writeAll("{\"error\":\"");
        self.resp_buf.writeJsonString(msg);
        self.resp_buf.writeAll("\"}");
        self.sendResponse(w, slot, c, status, self.resp_buf.slice(), keep_alive);
    }

    pub inline fn writeHeaderDirect(buf: *[128]u8, status: u16, body_len: usize, keep_alive: bool) usize {
        var off: usize = 0;
        if (status == 200) {
            const s = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
            @memcpy(buf[0..s.len], s);
            off = s.len;
        } else if (status == 201) {
            const s = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n";
            @memcpy(buf[0..s.len], s);
            off = s.len;
        } else {
            const line = switch (status) {
                400 => "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n",
                401 => "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n",
                404 => "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n",
                503 => "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n",
                else => "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n",
            };
            @memcpy(buf[0..line.len], line);
            off = line.len;
        }

        if (!keep_alive) {
            const s = "Connection: close\r\n";
            @memcpy(buf[off..][0..s.len], s);
            off += s.len;
        }

        const cl = "Content-Length: ";
        @memcpy(buf[off..][0..cl.len], cl);
        off += cl.len;

        off += bufm.writeU64Direct(buf[off..], body_len);

        const end = "\r\n\r\n";
        @memcpy(buf[off..][0..end.len], end);
        off += end.len;

        return off;
    }

    fn sendResponse(self: *Server, w: *RingWorker, slot: u32, c: *Conn, status: u16, body: []const u8, keep_alive: bool) void {
        var hdr_buf: [128]u8 = undefined;
        const hdr_len = writeHeaderDirect(&hdr_buf, status, body.len, keep_alive);
        const total_len = hdr_len + body.len;

        var out_ptr: [*]u8 = undefined;
        var is_pool = false;

        if (total_len <= pool_buf_size) {
            if (self.resp_pool.acquire()) |p| {
                out_ptr = p.ptr;
                is_pool = true;
            } else {
                const mem = gpa.alloc(u8, total_len) catch {
                    self.finishConn(w, slot, c);
                    return;
                };
                out_ptr = mem.ptr;
            }
        } else {
            const mem = gpa.alloc(u8, total_len) catch {
                self.finishConn(w, slot, c);
                return;
            };
            out_ptr = mem.ptr;
        }

        @memcpy(out_ptr[0..hdr_len], hdr_buf[0..hdr_len]);
        @memcpy((out_ptr + hdr_len)[0..body.len], body);

        c.out = out_ptr;
        c.out_len = @intCast(total_len);
        c.out_off = 0;
        c.setOutPool(is_pool);

        self.markTouched(@intCast(w.id), slot, c);
    }

    fn armSend(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        const s = getSqe(w) orelse return;
        const slice = c.out.?[c.out_off..c.out_len];

        // Conditional Zero-Copy Send:
        // For large payloads (>= 8 KB, e.g. /feed), use IORING_OP_SEND_ZC to bypass kernel skb copy.
        // For small payloads (< 8 KB), standard copy send is faster and avoids 2-CQE event handling overhead.
        if (slice.len >= 8192) {
            s.prep_send_zc(@intCast(slot), slice, linux.MSG.NOSIGNAL, 0);
            c.setZcPending(true);
        } else {
            s.prep_send(@intCast(slot), slice, linux.MSG.NOSIGNAL);
            c.setZcPending(false);
        }

        s.flags |= linux.IOSQE_FIXED_FILE;
        s.user_data = ud(.send, c.gen, slot);
        c.setSendLive(true);
    }

    fn onSend(self: *Server, w: *RingWorker, slot: u32, gen: u32, res: i32, cqe_flags: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (!c.isOpen() or c.gen != gen) return;

        // Check if this CQE is the zerocopy notification
        if (cqe_flags & linux.IORING_CQE_F_NOTIF != 0) {
            c.setZcPending(false);
            if (c.isClosing() or c.isCloseAfter()) {
                self.freeOutput(c);
                self.armClose(w, slot, c);
                return;
            }
            if (c.out_off >= c.out_len and !c.isSendLive()) {
                self.freeOutput(c);
                if (c.pend_len > 0) {
                    self.serve(w, slot, c, &.{});
                }
            }
            return;
        }

        c.setSendLive(false);

        // If IORING_CQE_F_MORE is not set, no notification will follow.
        if (cqe_flags & linux.IORING_CQE_F_MORE == 0) {
            c.setZcPending(false);
        }

        if (res <= 0) {
            if (!c.isZcPending()) {
                self.freeOutput(c);
            }
            self.finishConn(w, slot, c);
            return;
        }

        c.out_off += @intCast(res);
        if (c.out_off < c.out_len) {
            self.armSend(w, slot, c);
            return;
        }

        // Response fully sent
        if (!c.isZcPending()) {
            self.freeOutput(c);
        }

        if (c.isCloseAfter()) {
            if (!c.isZcPending()) {
                self.armClose(w, slot, c);
            }
        } else if (!c.isZcPending() and c.pend_len > 0) {
            self.serve(w, slot, c, &.{});
        }
    }

    fn freeOutput(self: *Server, c: *Conn) void {
        if (c.out) |m| {
            if (c.isOutPool()) {
                self.resp_pool.release(m);
            } else {
                gpa.free(m[0..c.out_len]);
            }
            c.out = null;
        }
        c.out_len = 0;
        c.out_off = 0;
        c.setOutPool(false);
    }

    fn armClose(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        c.setClosing(true);
        if (c.isZcPending()) {
            // Buffer is still held by kernel for zerocopy send; wait for NOTIF CQE before shutdown/close
            return;
        }
        const s1 = getSqe(w) orelse return;
        s1.prep_shutdown(@intCast(slot), linux.SHUT.RDWR);
        s1.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_IO_HARDLINK;
        s1.user_data = ud(.shut, c.gen, slot);

        const s2 = getSqe(w) orelse return;
        s2.prep_close_direct(slot);
        s2.user_data = ud(.close, c.gen, slot);
    }

    fn onShut(self: *Server, w: *RingWorker, slot: u32, gen: u32) void {
        _ = self;
        _ = w;
        _ = slot;
        _ = gen;
    }

    fn onClose(self: *Server, w: *RingWorker, slot: u32, gen: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (c.gen == gen and c.isOpen()) {
            if (!c.isZcPending()) {
                self.freeOutput(c);
            }
            if (c.pend_buf) |p| {
                gpa.free(p[0..c.pend_cap]);
                c.pend_buf = null;
                c.pend_cap = 0;
            }
            c.pend_len = 0;
            c.gen = (c.gen +% 1) & gen_mask;
            c.flags = 0;
            w.nconn -%= 1;

            if (w.full and w.nconn + 2 * w.margin < w.slots) {
                w.full = false;
                if (!w.accepting) {
                    self.armAccepts(w);
                }
            }
        }
    }

    fn isJsWhitespace(s: []const u8) ?usize {
        if (s.len == 0) return null;
        const c0 = s[0];
        if (c0 == ' ' or (c0 >= 9 and c0 <= 13)) return 1;
        if (s.len >= 2 and s[0] == 0xc2 and s[1] == 0xa0) return 2;
        if (s.len >= 3) {
            const u = (@as(u32, s[0]) << 16) | (@as(u32, s[1]) << 8) | @as(u32, s[2]);
            if (u == 0xe19a80 or (u >= 0xe28080 and u <= 0xe2808a) or u == 0xe280a8 or u == 0xe280a9 or u == 0xe280af or u == 0xe2819f or u == 0xe38080 or u == 0xefbbbf) return 3;
        }
        return null;
    }

    pub fn trimJs(s: []const u8) []const u8 {
        var start: usize = 0;
        while (start < s.len) {
            if (isJsWhitespace(s[start..])) |len| {
                start += len;
            } else break;
        }
        var end: usize = s.len;
        while (end > start) {
            var step: usize = 1;
            if (end >= start + 3 and isJsWhitespace(s[end - 3 .. end]) == 3) {
                step = 3;
            } else if (end >= start + 2 and isJsWhitespace(s[end - 2 .. end]) == 2) {
                step = 2;
            } else if (isJsWhitespace(s[end - 1 .. end]) == 1) {
                step = 1;
            } else break;
            end -= step;
        }
        return s[start..end];
    }

    pub fn parseId(s: []const u8) ?i64 {
        if (s.len == 0 or s.len > 18) return null;
        var v: i64 = 0;
        for (s) |ch| {
            const d = ch -% '0';
            if (d > 9) return null;
            v = v * 10 + d;
        }
        return if (v > 0) v else null;
    }
};
