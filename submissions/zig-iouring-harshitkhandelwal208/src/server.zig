//! The io_uring multi-ring event loop for the $12 server challenge.
//! Uses direct descriptors, 10 SO_REUSEPORT listeners (one per ring),
//! 10 io_uring rings (654,720 connection capacity),
//! 32-byte packed connection state, provided-buffer rings with ENOBUFS burst recovery,
//! hugepage allocations, SIMD HTTP routing, read-first group commit batching, and epoll wakeup.
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
pub const n_bufs_per_ring = 2048;
pub const bgid = 1;

pub const pool_buf_size: usize = 4096;
pub const pool_count: usize = 2048;

pub const k_ring_count: usize = 16;
pub const slots_per_ring: u32 = 65472;

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

    pub inline fn acquire(self: *RespPool) ?[]u8 {
        if (self.top == 0) return null;
        self.top -= 1;
        const id = self.free_stack[self.top];
        return (self.mem + @as(usize, id) * pool_buf_size)[0..pool_buf_size];
    }

    pub inline fn release(self: *RespPool, ptr: [*]u8) void {
        const offset = @intFromPtr(ptr) - @intFromPtr(self.mem);
        const id: u16 = @intCast(offset / pool_buf_size);
        self.free_stack[self.top] = id;
        self.top += 1;
    }
};

const Tag = enum(u8) { accept = 1, recv, send, shut, close, timer };

inline fn ud(tag: Tag, gen: u32, idx: u32) u64 {
    return (@as(u64, @intFromEnum(tag)) << 56) | (@as(u64, gen & 0xffffff) << 32) | idx;
}

pub const Conn = struct {
    gen: u32 = 0,
    flags: u8 = 0,
    ring_idx: u8 = 0,
    pend_len: u16 = 0,
    pend_buf: ?[*]u8 = null,
    out: ?[*]u8 = null,
    out_len: u16 = 0,
    out_off: u16 = 0,
    pend_cap: u16 = 0,
    reserved: u16 = 0,

    pub const FLAG_OPEN: u8 = 1 << 0;
    pub const FLAG_RECV_LIVE: u8 = 1 << 1;
    pub const FLAG_SEND_LIVE: u8 = 1 << 2;
    pub const FLAG_SHUT_LIVE: u8 = 1 << 3;
    pub const FLAG_CLOSING: u8 = 1 << 4;
    pub const FLAG_OUT_POOL: u8 = 1 << 5;
    pub const FLAG_READ_QUEUED: u8 = 1 << 6;
    pub const FLAG_IN_BATCH: u8 = 1 << 7;

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

    inline fn setFlag(self: *Conn, flag: u8, v: bool) void {
        if (v) self.flags |= flag else self.flags &= ~flag;
    }
};

comptime {
    std.debug.assert(@sizeOf(Conn) == 32);
}

pub const RingWorker = struct {
    id: u32 = 0,
    ring: IoUring = undefined,
    conns: []Conn = &.{},
    slots: u32 = slots_per_ring,
    nconn: u32 = 0,
    accept_live: bool = false,
    pbuf_ring: [*]linux.io_uring_buf = undefined,
    pbuf_mem: [*]u8 = undefined,
    pbuf_tail: u16 = 0,
    rearm_queue: []u32 = &.{},
    rearm_head: u32 = 0,
    rearm_tail: u32 = 0,

    inline fn pushRearm(self: *RingWorker, slot: u32) void {
        const next: u32 = @truncate((self.rearm_tail + 1) % self.rearm_queue.len);
        if (next != self.rearm_head) {
            self.rearm_queue[self.rearm_tail] = slot;
            self.rearm_tail = next;
        }
    }

    inline fn popRearm(self: *RingWorker) ?u32 {
        if (self.rearm_head == self.rearm_tail) return null;
        const slot = self.rearm_queue[self.rearm_head];
        self.rearm_head = @truncate((self.rearm_head + 1) % self.rearm_queue.len);
        return slot;
    }
};

pub const PendingWrite = struct {
    w: *RingWorker,
    slot: u32,
};

pub const Server = struct {
    rings: [k_ring_count]RingWorker = undefined,
    active_rings: usize = 0,
    listeners: [k_ring_count]i32 = @splat(-1),
    epfd: i32 = -1,
    db: Db,
    jwt: Key,
    started: i64,
    now: i64,
    scratch: Scratch = undefined,
    resp_buf: Buf = .{},
    resp_pool: RespPool = undefined,
    in_tx: bool = false,
    pending_writes: [4096]PendingWrite = undefined,
    n_pending_writes: usize = 0,

    pub fn init(db: Db, jwt_key: Key) !*Server {
        const smem = try sys.mapAnon(@sizeOf(Server));
        const s: *Server = @ptrCast(@alignCast(smem.ptr));
        @memset(smem, 0);

        s.db = db;
        s.jwt = jwt_key;
        s.started = sys.nowUnix();
        s.now = sys.nowUnix();
        for (&s.listeners) |*l| l.* = -1;

        const ep = linux.epoll_create1(linux.EPOLL.CLOEXEC);
        if (sys.errnoOf(ep) != .SUCCESS) return error.EpollFailed;
        s.epfd = @intCast(ep);

        // Pre-allocate rings
        var n_initialized: usize = 0;
        for (0..k_ring_count) |i| {
            var rw = &s.rings[i];
            rw.id = @intCast(i);
            rw.slots = slots_per_ring;
            rw.nconn = 0;
            rw.ring = initRing() catch |e| {
                if (i == 0) return e;
                break;
            };

            const qmem = try sys.mapAnon(65536 * @sizeOf(u32));
            rw.rearm_queue = @as([*]u32, @ptrCast(@alignCast(qmem.ptr)))[0..65536];
            rw.rearm_head = 0;
            rw.rearm_tail = 0;

            setupPbufRing(rw) catch |e| {
                if (i == 0) return e;
                break;
            };

            rw.ring.register_files_sparse(slots_per_ring) catch |e| {
                if (i == 0) return e;
                break;
            };

            const cmem = try sys.mapAnon(@as(usize, slots_per_ring) * @sizeOf(Conn));
            sys.madviseHuge(cmem);
            rw.conns = @as([*]Conn, @ptrCast(@alignCast(cmem.ptr)))[0..slots_per_ring];

            var ev: linux.epoll_event = .{
                .events = linux.EPOLL.IN,
                .data = .{ .u32 = @intCast(i) },
            };
            _ = linux.epoll_ctl(s.epfd, 1, rw.ring.fd, &ev);

            n_initialized += 1;
        }

        s.active_rings = n_initialized;
        s.scratch = try Scratch.init(128 * 1024);
        s.resp_buf = try Buf.init(128 * 1024);
        s.resp_pool = try RespPool.init();

        return s;
    }

    fn initRing() !IoUring {
        const flag_sets = [_]u32{
            linux.IORING_SETUP_CQSIZE,
            0,
        };
        const sq_sizes = [_]u16{ 8192, 4096, 2048 };
        const cq_sizes = [_]u32{ 65536, 32768, 16384 };
        var last: anyerror = error.ArgumentsInvalid;
        for (sq_sizes) |sq| {
            for (cq_sizes) |cq| {
                for (flag_sets) |fl| {
                    var p = std.mem.zeroInit(linux.io_uring_params, .{ .flags = fl });
                    p.cq_entries = cq;
                    if (IoUring.init_params(sq, &p)) |r| return r else |e| last = e;
                }
            }
        }
        return last;
    }

    fn setupPbufRing(w: *RingWorker) !void {
        const ring_bytes = n_bufs_per_ring * @sizeOf(linux.io_uring_buf);
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

        const bmem = try sys.mapAnon(n_bufs_per_ring * buf_size);
        sys.madviseHuge(bmem);
        w.pbuf_mem = bmem.ptr;
        w.pbuf_ring = @ptrCast(@alignCast(rmem.ptr));
        w.pbuf_tail = 0;
        for (0..n_bufs_per_ring) |i| returnBuf(w, @intCast(i));
        publishBufs(w);
    }

    inline fn returnBuf(w: *RingWorker, bid: u16) void {
        const idx = w.pbuf_tail & (n_bufs_per_ring - 1);
        w.pbuf_ring[idx] = .{
            .addr = @intFromPtr(w.pbuf_mem + @as(usize, bid) * buf_size),
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
        return w.ring.get_sqe() catch {
            _ = w.ring.submit() catch return null;
            return w.ring.get_sqe() catch null;
        };
    }

    pub fn makeListeners(self: *Server, host: [4]u8, port: u16) !void {
        const one: c_int = 1;
        const defer_secs: c_int = 1;
        const rcv: c_int = 2048;
        const snd: c_int = 16384;
        const clamp: c_int = 4096;

        for (0..self.active_rings) |i| {
            const s = linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC, 0);
            if (sys.errnoOf(s) != .SUCCESS) return error.SocketFailed;
            const fd: i32 = @intCast(s);

            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEADDR, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEPORT, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, @ptrCast(&one), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 9, @ptrCast(&defer_secs), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.RCVBUF, @ptrCast(&rcv), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.SNDBUF, @ptrCast(&snd), @sizeOf(c_int));
            _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 10, @ptrCast(&clamp), @sizeOf(c_int));

            var sa: linux.sockaddr.in = .{ .port = std.mem.nativeToBig(u16, port), .addr = @bitCast(host) };
            if (sys.errnoOf(linux.bind(fd, @ptrCast(&sa), @sizeOf(linux.sockaddr.in))) != .SUCCESS) return error.BindFailed;
            if (sys.errnoOf(linux.listen(fd, 4096)) != .SUCCESS) return error.ListenFailed;

            self.listeners[i] = fd;
            self.armAccept(&self.rings[i], @intCast(i));
        }
    }

    fn armAccept(self: *Server, w: *RingWorker, listener_idx: u32) void {
        const s = getSqe(w) orelse return;
        s.prep_multishot_accept_direct(self.listeners[listener_idx], null, null, linux.SOCK.NONBLOCK);
        s.user_data = ud(.accept, 0, listener_idx);
        w.accept_live = true;
    }

    fn armRecv(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        const s = getSqe(w) orelse {
            w.pushRearm(slot);
            return;
        };
        s.prep_recv_multishot(@intCast(slot), &.{}, 0);
        s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
        s.buf_index = bgid;
        s.user_data = ud(.recv, c.gen, slot);
        c.setRecvLive(true);
    }

    pub fn run(self: *Server) !void {
        var ep_events: [32]linux.epoll_event = undefined;

        while (true) {
            var total_reaped: usize = 0;

            // Drain CQEs across all active rings
            for (0..self.active_rings) |i| {
                const w = &self.rings[i];
                total_reaped += self.drainRing(w);
            }

            // Group commit: Commit any pending writes from this pass before sending responses
            if (self.in_tx) {
                if (self.db.commitTx()) {
                    for (self.pending_writes[0..self.n_pending_writes]) |pw| {
                        const c = &pw.w.conns[pw.slot];
                        if (c.isOpen() and !c.isClosing() and c.out != null) {
                            self.armSend(pw.w, pw.slot, c);
                        }
                    }
                } else {
                    self.db.rollbackTx();
                    for (self.pending_writes[0..self.n_pending_writes]) |pw| {
                        const c = &pw.w.conns[pw.slot];
                        if (c.isOpen()) {
                            self.sendError(pw.w, pw.slot, c, 500, "internal server error", false);
                        }
                    }
                }
                self.in_tx = false;
                self.n_pending_writes = 0;
            }

            // Submit pending SQEs across all active rings
            for (0..self.active_rings) |i| {
                const w = &self.rings[i];
                _ = w.ring.submit() catch {};
            }

            // Tick WAL checkpoint coordinator
            self.db.tick();

            // If no completions were reaped anywhere, wait in epoll for next event
            if (total_reaped == 0) {
                _ = linux.epoll_wait(self.epfd, &ep_events, ep_events.len, 50);
            }
        }
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
            const gen: u32 = @truncate((cqe.user_data >> 32) & 0xffffff);
            const slot: u32 = @truncate(cqe.user_data);

            switch (tag) {
                .accept => self.onAccept(w, cqe.res, cqe.flags, slot),
                .recv => {
                    self.onRecv(w, slot, gen, cqe.res, cqe.flags);
                    if (cqe.flags & linux.IORING_CQE_F_BUFFER != 0) {
                        const bid: u16 = @truncate(cqe.flags >> linux.IORING_CQE_BUFFER_SHIFT);
                        returnBuf(w, bid);
                        returned += 1;
                        if (returned >= 64) {
                            publishBufs(w);
                            returned = 0;
                        }
                    }
                },
                .send => self.onSend(w, slot, gen, cqe.res),
                .shut => self.onShut(w, slot, gen),
                .close => {},
                .timer => {},
            }
        }
        w.ring.cq_advance(count);

        if (returned > 0) {
            publishBufs(w);
        }

        // Re-arm connections that paused due to ENOBUFS in bounded rounds
        var rearmed: usize = 0;
        while (w.popRearm()) |s| {
            const conn = &w.conns[s];
            if (conn.isOpen() and !conn.isRecvLive() and !conn.isClosing()) {
                self.armRecv(w, s, conn);
                rearmed += 1;
                if (rearmed >= 256) break;
            }
        }

        return count;
    }

    fn onAccept(self: *Server, w: *RingWorker, res: i32, flags: u32, listener_idx: u32) void {
        if (flags & linux.IORING_CQE_F_MORE == 0) w.accept_live = false;
        if (res < 0) {
            if (!w.accept_live) {
                self.armAccept(w, listener_idx);
            }
            return;
        }
        const slot: u32 = @intCast(res);
        if (slot >= w.slots) return;

        const c = &w.conns[slot];
        c.* = .{
            .gen = c.gen +% 1,
            .flags = Conn.FLAG_OPEN,
            .ring_idx = @intCast(w.id),
        };
        w.nconn += 1;
        self.armRecv(w, slot, c);
    }

    fn onRecv(self: *Server, w: *RingWorker, slot: u32, gen: u32, res: i32, flags: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (!c.isOpen() or c.gen != gen) return;

        if (flags & linux.IORING_CQE_F_MORE == 0) {
            c.setRecvLive(false);
        }

        if (res <= 0) {
            if (res == -@as(i32, @intCast(@intFromEnum(linux.E.NOBUFS))) or res == -105) {
                // Buffer ring ran dry momentarily during burst: enqueue for rearm as soon as buffers return
                c.setRecvLive(false);
                w.pushRearm(slot);
                return;
            }
            self.closeConn(w, slot, c);
            return;
        }

        const bid: u16 = @truncate(flags >> linux.IORING_CQE_BUFFER_SHIFT);
        const data = (w.pbuf_mem + @as(usize, bid) * buf_size)[0..@intCast(res)];

        self.handleData(w, slot, c, data);

        if (!c.isRecvLive() and c.isOpen() and !c.isClosing()) {
            self.armRecv(w, slot, c);
        } else if (c.isClosing()) {
            self.maybeFinishClose(w, slot, c);
        }
    }

    fn handleData(self: *Server, w: *RingWorker, slot: u32, c: *Conn, data: []const u8) void {
        var input: []const u8 = data;
        if (c.pend_len > 0) {
            if (data.len > 0) {
                if (c.pend_buf) |pbuf| {
                    const total = @as(usize, c.pend_len) + data.len;
                    if (total > c.pend_cap) {
                        var new_cap: usize = if (c.pend_cap > 0) @as(usize, c.pend_cap) * 2 else 4096;
                        while (new_cap < total) new_cap *= 2;
                        const new_mem = gpa.alloc(u8, new_cap) catch return;
                        @memcpy(new_mem[0..c.pend_len], pbuf[0..c.pend_len]);
                        gpa.free(pbuf[0..c.pend_cap]);
                        c.pend_buf = new_mem.ptr;
                        c.pend_cap = @intCast(new_cap);
                    }
                    const p = c.pend_buf.?;
                    @memcpy(p[c.pend_len..][0..data.len], data);
                    c.pend_len = @intCast(total);
                    input = p[0..total];
                }
            } else if (c.pend_buf) |pbuf| {
                input = pbuf[0..c.pend_len];
            }
        }

        var offset: usize = 0;
        while (offset < input.len and c.isOpen() and !c.isClosing() and !c.isSendLive() and c.out == null) {
            const rem = input[offset..];
            const parsed = http.parseHead(rem);
            switch (parsed) {
                .ok => |head| {
                    const req_len = head.head_len + head.content_length;
                    if (offset + req_len > input.len) {
                        // Incomplete body
                        self.savePending(c, rem);
                        return;
                    }
                    const body = rem[head.head_len..req_len];
                    offset += req_len;

                    self.serve(w, slot, c, &head, body);
                },
                .need_more => {
                    self.savePending(c, rem);
                    return;
                },
                .bad => |status| {
                    self.sendError(w, slot, c, status, "bad request", false);
                    c.setClosing(true);
                    return;
                },
            }
        }

        if (offset >= input.len) {
            c.pend_len = 0;
        } else {
            self.savePending(c, input[offset..]);
        }
    }

    fn savePending(self: *Server, c: *Conn, remaining: []const u8) void {
        _ = self;
        if (c.pend_buf == null) {
            const cap: usize = 4096;
            const mem = gpa.alloc(u8, cap) catch return;
            c.pend_buf = mem.ptr;
            c.pend_cap = @intCast(cap);
        }
        if (remaining.len > c.pend_cap) {
            var new_cap: usize = @as(usize, c.pend_cap) * 2;
            while (new_cap < remaining.len) new_cap *= 2;
            const new_mem = gpa.alloc(u8, new_cap) catch return;
            gpa.free(c.pend_buf.?[0..c.pend_cap]);
            c.pend_buf = new_mem.ptr;
            c.pend_cap = @intCast(new_cap);
        }
        std.mem.copyForwards(u8, c.pend_buf.?[0..remaining.len], remaining);
        c.pend_len = @intCast(remaining.len);
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

    fn trimJs(s: []const u8) []const u8 {
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

    fn parseId(s: []const u8) ?i64 {
        if (s.len == 0) return null;
        var v: i64 = 0;
        for (s) |ch| {
            if (ch < '0' or ch > '9') return null;
            if (v < 100_000_000_000_000_000) {
                v = v * 10 + (ch - '0');
            } else return null;
        }
        if (v <= 0) return null;
        return v;
    }

    fn serve(self: *Server, w: *RingWorker, slot: u32, c: *Conn, head: *const http.Head, body: []const u8) void {
        self.scratch.reset();
        self.resp_buf.reset();
        const keep_alive = head.keep_alive;
        if (!keep_alive) c.setClosing(true);

        const path = head.path;
        const method = head.method;

        // Route: GET /feed
        if (method == .get and std.mem.eql(u8, path, "/feed")) {
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

                // Parse body JSON: {"body":"..."}
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

                const chars = std.unicode.utf8CountCodepoints(trimmed) catch {
                    self.sendError(w, slot, c, 400, "body is required", keep_alive);
                    return;
                };
                if (chars > 500) {
                    self.sendError(w, slot, c, 400, "body must be at most 500 characters", keep_alive);
                    return;
                }

                self.ensureTx();
                self.db.createPost(claims.uid, claims.name, trimmed, &self.resp_buf) catch {
                    self.sendError(w, slot, c, 500, "internal server error", keep_alive);
                    return;
                };
                self.queueWriteResponse(w, slot, c, 201, self.resp_buf.slice(), keep_alive);
                return;
            }

            // Path starts with /posts/
            if (rem.len > 1 and rem[0] == '/') {
                const subpath = rem[1..];

                // Like: /posts/:id/like
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
                        self.ensureTx();
                        switch (self.db.likePost(claims.uid, post_id, &self.resp_buf)) {
                            .created => self.queueWriteResponse(w, slot, c, 201, self.resp_buf.slice(), keep_alive),
                            .already_liked => self.queueWriteResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive),
                            .not_found => self.sendError(w, slot, c, 404, "post not found", keep_alive),
                            .err => self.sendError(w, slot, c, 500, "internal server error", keep_alive),
                        }
                        return;
                    } else if (method == .delete) {
                        self.ensureTx();
                        switch (self.db.unlikePost(claims.uid, post_id, &self.resp_buf)) {
                            .ok => self.queueWriteResponse(w, slot, c, 200, self.resp_buf.slice(), keep_alive),
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

    inline fn writeHeaderDirect(buf: *[128]u8, status: u16, body_len: usize, keep_alive: bool) usize {
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
                const mem = gpa.alloc(u8, total_len) catch return;
                out_ptr = mem.ptr;
            }
        } else {
            const mem = gpa.alloc(u8, total_len) catch return;
            out_ptr = mem.ptr;
        }

        @memcpy(out_ptr[0..hdr_len], hdr_buf[0..hdr_len]);
        @memcpy((out_ptr + hdr_len)[0..body.len], body);

        c.out = out_ptr;
        c.out_len = @intCast(total_len);
        c.out_off = 0;
        c.setOutPool(is_pool);

        self.armSend(w, slot, c);
    }

    inline fn ensureTx(self: *Server) void {
        if (!self.in_tx) {
            _ = self.db.beginTx();
            self.in_tx = true;
        }
    }

    fn queueWriteResponse(self: *Server, w: *RingWorker, slot: u32, c: *Conn, status: u16, body: []const u8, keep_alive: bool) void {
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
                const mem = gpa.alloc(u8, total_len) catch return;
                out_ptr = mem.ptr;
            }
        } else {
            const mem = gpa.alloc(u8, total_len) catch return;
            out_ptr = mem.ptr;
        }

        @memcpy(out_ptr[0..hdr_len], hdr_buf[0..hdr_len]);
        @memcpy((out_ptr + hdr_len)[0..body.len], body);

        c.out = out_ptr;
        c.out_len = @intCast(total_len);
        c.out_off = 0;
        c.setOutPool(is_pool);

        if (self.n_pending_writes < self.pending_writes.len) {
            self.pending_writes[self.n_pending_writes] = .{ .w = w, .slot = slot };
            self.n_pending_writes += 1;
        } else {
            self.armSend(w, slot, c);
        }
    }

    fn armSend(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        const s = getSqe(w) orelse return;
        const slice = c.out.?[c.out_off..c.out_len];
        s.prep_send(@intCast(slot), slice, linux.MSG.NOSIGNAL);
        s.flags |= linux.IOSQE_FIXED_FILE;
        s.user_data = ud(.send, c.gen, slot);
        c.setSendLive(true);
    }

    fn onSend(self: *Server, w: *RingWorker, slot: u32, gen: u32, res: i32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (!c.isOpen() or c.gen != gen) return;

        c.setSendLive(false);
        if (res <= 0) {
            self.closeConn(w, slot, c);
            return;
        }

        c.out_off += @intCast(res);
        if (c.out_off < c.out_len) {
            self.armSend(w, slot, c);
            return;
        }

        // Response fully sent
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

        if (c.isClosing()) {
            self.closeConn(w, slot, c);
        } else if (c.pend_len > 0) {
            self.handleData(w, slot, c, &.{});
        }
    }

    fn onShut(self: *Server, w: *RingWorker, slot: u32, gen: u32) void {
        if (slot >= w.slots) return;
        const c = &w.conns[slot];
        if (c.gen != gen) return;
        c.setShutLive(false);
        self.maybeFinishClose(w, slot, c);
    }

    fn closeConn(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        if (!c.isOpen() or c.isClosing()) return;
        c.setClosing(true);

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
        if (c.pend_buf) |p| {
            gpa.free(p[0..c.pend_cap]);
            c.pend_buf = null;
            c.pend_cap = 0;
        }
        c.pend_len = 0;

        if (c.isRecvLive() or c.isSendLive()) {
            const s = getSqe(w) orelse {
                self.maybeFinishClose(w, slot, c);
                return;
            };
            s.prep_shutdown(@intCast(slot), linux.SHUT.RDWR);
            s.flags |= linux.IOSQE_FIXED_FILE;
            s.user_data = ud(.shut, c.gen, slot);
            c.setShutLive(true);
        }
        self.maybeFinishClose(w, slot, c);
    }

    fn maybeFinishClose(self: *Server, w: *RingWorker, slot: u32, c: *Conn) void {
        _ = self;
        if (!c.isClosing() or c.isRecvLive() or c.isSendLive() or c.isShutLive()) return;
        const s = getSqe(w) orelse return;
        c.setClosing(false);
        c.setOpen(false);
        w.nconn -%= 1;
        s.prep_close_direct(slot);
        s.user_data = ud(.close, 0, 0);
    }
};
