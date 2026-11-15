//! The io_uring event loop for the $12 server challenge.
//! Uses direct descriptors, multishot accept, multishot recv on a provided-buffer ring,
//! Transparent Hugepage SQLite pagecache, and fast SIMD routing.
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
pub const n_bufs = 16384;
pub const bgid = 1;

const Tag = enum(u8) { accept = 1, recv, send, shut, close, timer };

inline fn ud(tag: Tag, gen: u32, idx: u32) u64 {
    return (@as(u64, @intFromEnum(tag)) << 56) | (@as(u64, gen & 0xffffff) << 32) | idx;
}

pub const Conn = struct {
    gen: u32 = 0,
    open: bool = false,
    recv_live: bool = false,
    send_live: bool = false,
    shut_live: bool = false,
    closing: bool = false,
    pend_buf: ?[]u8 = null,
    pend_len: usize = 0,
    out: ?[]u8 = null,
    out_len: usize = 0,
    out_off: usize = 0,
};

pub const Server = struct {
    ring: IoUring = undefined,
    db: Db,
    jwt: Key,
    listener: i32 = -1,
    conns: []Conn = &.{},
    slots: u32,
    started: i64,
    now: i64,
    accept_live: bool = false,
    pbuf_ring: [*]linux.io_uring_buf = undefined,
    pbuf_mem: [*]u8 = undefined,
    pbuf_tail: u16 = 0,
    scratch: Scratch = undefined,
    resp_buf: Buf = .{},
    head_buf: Buf = .{},

    pub fn init(db: Db, jwt_key: Key, slots: u32) !*Server {
        const s = try gpa.create(Server);
        s.* = .{
            .db = db,
            .jwt = jwt_key,
            .slots = slots,
            .started = sys.nowUnix(),
            .now = sys.nowUnix(),
        };

        s.ring = try initRing();
        try setupPbufRing(s);

        s.ring.register_files_sparse(slots) catch |e| {
            log.err("cannot register {d} direct descriptors ({s})", .{ slots, @errorName(e) });
            return e;
        };

        const cmem = try sys.mapAnon(@as(usize, slots) * @sizeOf(Conn));
        sys.madviseHuge(cmem);
        s.conns = @as([*]Conn, @ptrCast(@alignCast(cmem.ptr)))[0..slots];
        @memset(s.conns, Conn{});

        s.scratch = try Scratch.init(128 * 1024);
        s.resp_buf = try Buf.init(128 * 1024);
        s.head_buf = try Buf.init(4096);

        return s;
    }

    fn initRing() !IoUring {
        const flag_sets = [_]u32{
            linux.IORING_SETUP_SINGLE_ISSUER | linux.IORING_SETUP_DEFER_TASKRUN | linux.IORING_SETUP_CQSIZE,
            linux.IORING_SETUP_COOP_TASKRUN | linux.IORING_SETUP_CQSIZE,
            linux.IORING_SETUP_CQSIZE,
        };
        const cq_sizes = [_]u32{ 65536, 16384, 4096 };
        var last: anyerror = error.ArgumentsInvalid;
        for (cq_sizes) |cq| {
            for (flag_sets) |fl| {
                var p = std.mem.zeroInit(linux.io_uring_params, .{ .flags = fl });
                p.cq_entries = cq;
                if (IoUring.init_params(4096, &p)) |r| return r else |e| last = e;
            }
        }
        return last;
    }

    fn setupPbufRing(self: *Server) !void {
        const ring_bytes = n_bufs * @sizeOf(linux.io_uring_buf);
        const rmem = try sys.mapAnon(ring_bytes);
        var reg = std.mem.zeroInit(linux.io_uring_buf_reg, .{
            .ring_addr = @intFromPtr(rmem.ptr),
            .ring_entries = n_bufs,
            .bgid = bgid,
        });
        var rc = linux.io_uring_register(self.ring.fd, .REGISTER_PBUF_RING, &reg, 1);
        if (sys.errnoOf(rc) == .INVAL) {
            reg.resv[0] = 1;
            rc = linux.io_uring_register(self.ring.fd, .REGISTER_PBUF_RING, &reg, 1);
            reg.resv[0] = 0;
        }
        if (sys.errnoOf(rc) != .SUCCESS) return error.NoBufferRings;

        const bmem = try sys.mapAnon(n_bufs * buf_size);
        sys.madviseHuge(bmem);
        self.pbuf_mem = bmem.ptr;
        self.pbuf_ring = @ptrCast(@alignCast(rmem.ptr));
        self.pbuf_tail = 0;
        for (0..n_bufs) |i| self.returnBuf(@intCast(i));
        self.publishBufs();
    }

    inline fn returnBuf(self: *Server, bid: u16) void {
        const idx = self.pbuf_tail & (n_bufs - 1);
        self.pbuf_ring[idx] = .{
            .addr = @intFromPtr(self.pbuf_mem + @as(usize, bid) * buf_size),
            .len = buf_size,
            .bid = bid,
            .resv = 0,
        };
        self.pbuf_tail +%= 1;
    }

    inline fn publishBufs(self: *Server) void {
        const br: *linux.io_uring_buf_ring = @ptrCast(self.pbuf_ring);
        @atomicStore(u16, &br.tail, self.pbuf_tail, .release);
    }

    pub fn makeListener(self: *Server, host: [4]u8, port: u16) !void {
        const s = linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC, 0);
        if (sys.errnoOf(s) != .SUCCESS) return error.SocketFailed;
        const fd: i32 = @intCast(s);
        const one: c_int = 1;
        _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEADDR, @ptrCast(&one), @sizeOf(c_int));
        _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEPORT, @ptrCast(&one), @sizeOf(c_int));
        _ = linux.setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, @ptrCast(&one), @sizeOf(c_int));

        // TCP_DEFER_ACCEPT: 1 second
        const defer_secs: c_int = 1;
        _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 9, @ptrCast(&defer_secs), @sizeOf(c_int));

        // Socket memory clamping: SO_RCVBUF=2048, SO_SNDBUF=16384, TCP_WINDOW_CLAMP=4096
        const rcv: c_int = 2048;
        _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.RCVBUF, @ptrCast(&rcv), @sizeOf(c_int));
        const snd: c_int = 16384;
        _ = linux.setsockopt(fd, linux.SOL.SOCKET, linux.SO.SNDBUF, @ptrCast(&snd), @sizeOf(c_int));
        const clamp: c_int = 4096;
        _ = linux.setsockopt(fd, linux.IPPROTO.TCP, 10, @ptrCast(&clamp), @sizeOf(c_int));

        var sa: linux.sockaddr.in = .{ .port = std.mem.nativeToBig(u16, port), .addr = @bitCast(host) };
        if (sys.errnoOf(linux.bind(fd, @ptrCast(&sa), @sizeOf(linux.sockaddr.in))) != .SUCCESS) return error.BindFailed;
        if (sys.errnoOf(linux.listen(fd, 65535)) != .SUCCESS) return error.ListenFailed;
        self.listener = fd;
    }

    fn armAccept(self: *Server) void {
        const s = self.ring.get_sqe() catch return;
        s.prep_multishot_accept_direct(self.listener, null, null, linux.SOCK.NONBLOCK);
        s.user_data = ud(.accept, 0, 0);
        self.accept_live = true;
    }

    fn armRecv(self: *Server, slot: u32, c: *Conn) void {
        const s = self.ring.get_sqe() catch return;
        s.prep_recv_multishot(@intCast(slot), &.{}, 0);
        s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
        s.buf_index = bgid;
        s.user_data = ud(.recv, c.gen, slot);
        c.recv_live = true;
    }

    pub fn run(self: *Server) !void {
        self.armAccept();

        var returned: usize = 0;
        while (true) {
            _ = self.ring.submit_and_wait(1) catch continue;

            const head = self.ring.cq.head.*;
            const tail = @atomicLoad(u32, self.ring.cq.tail, .acquire);
            const count = tail -% head;
            if (count == 0) continue;

            const mask = self.ring.cq.mask;
            var cur = head;
            while (cur != tail) : (cur +%= 1) {
                const cqe = &self.ring.cq.cqes[cur & mask];
                const tag: Tag = @enumFromInt(@as(u8, @truncate(cqe.user_data >> 56)));
                const gen: u32 = @truncate((cqe.user_data >> 32) & 0xffffff);
                const slot: u32 = @truncate(cqe.user_data);

                switch (tag) {
                    .accept => self.onAccept(cqe.res, cqe.flags),
                    .recv => {
                        self.onRecv(slot, gen, cqe.res, cqe.flags);
                        if (cqe.flags & linux.IORING_CQE_F_BUFFER != 0) {
                            const bid: u16 = @truncate(cqe.flags >> linux.IORING_CQE_BUFFER_SHIFT);
                            self.returnBuf(bid);
                            returned += 1;
                        }
                    },
                    .send => self.onSend(slot, gen, cqe.res),
                    .shut => self.onShut(slot, gen),
                    .close => {},
                    .timer => {},
                }
            }
            self.ring.cq_advance(count);

            if (returned > 0) {
                self.publishBufs();
                returned = 0;
            }
        }
    }

    fn onAccept(self: *Server, res: i32, flags: u32) void {
        if (flags & linux.IORING_CQE_F_MORE == 0) self.accept_live = false;
        if (res < 0) {
            if (!self.accept_live) self.armAccept();
            return;
        }
        const slot: u32 = @intCast(res);
        if (slot >= self.slots) return;

        const c = &self.conns[slot];
        c.* = .{
            .gen = c.gen +% 1,
            .open = true,
        };
        self.armRecv(slot, c);
    }

    fn onRecv(self: *Server, slot: u32, gen: u32, res: i32, flags: u32) void {
        if (slot >= self.slots) return;
        const c = &self.conns[slot];
        if (!c.open or c.gen != gen) return;

        if (flags & linux.IORING_CQE_F_MORE == 0) {
            c.recv_live = false;
        }

        if (res <= 0) {
            self.closeConn(slot, c);
            return;
        }

        const bid: u16 = @truncate(flags >> linux.IORING_CQE_BUFFER_SHIFT);
        const data = (self.pbuf_mem + @as(usize, bid) * buf_size)[0..@intCast(res)];

        self.handleData(slot, c, data);

        if (!c.recv_live and c.open and !c.closing) {
            self.armRecv(slot, c);
        } else if (c.closing) {
            self.maybeFinishClose(slot, c);
        }
    }

    fn handleData(self: *Server, slot: u32, c: *Conn, data: []const u8) void {
        var input: []const u8 = data;
        if (c.pend_len > 0) {
            if (c.pend_buf) |pbuf| {
                const total = c.pend_len + data.len;
                if (total <= pbuf.len) {
                    @memcpy(pbuf[c.pend_len..][0..data.len], data);
                    c.pend_len = total;
                    input = pbuf[0..total];
                }
            }
        }

        var offset: usize = 0;
        while (offset < input.len and c.open and !c.closing) {
            const parsed = http.parseHead(input[offset..]);
            switch (parsed) {
                .ok => |head| {
                    const req_len = head.head_len + head.content_length;
                    if (offset + req_len > input.len) {
                        // Incomplete body
                        self.savePending(c, input[offset..]);
                        return;
                    }
                    const body = input[offset + head.head_len .. offset + req_len];
                    offset += req_len;

                    self.serve(slot, c, &head, body);
                },
                .need_more => {
                    self.savePending(c, input[offset..]);
                    return;
                },
                .bad => |status| {
                    self.sendError(slot, c, status, "bad request", false);
                    c.closing = true;
                    return;
                },
            }
        }

        if (offset >= input.len) {
            c.pend_len = 0;
        }
    }

    fn savePending(self: *Server, c: *Conn, remaining: []const u8) void {
        _ = self;
        if (c.pend_buf == null) {
            c.pend_buf = gpa.alloc(u8, 64 * 1024) catch return;
        }
        if (remaining.len <= c.pend_buf.?.len) {
            @memcpy(c.pend_buf.?[0..remaining.len], remaining);
            c.pend_len = remaining.len;
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

    fn serve(self: *Server, slot: u32, c: *Conn, head: *const http.Head, body: []const u8) void {
        self.scratch.reset();
        self.resp_buf.reset();
        const keep_alive = head.keep_alive;
        if (!keep_alive) c.closing = true;

        const path = head.path;
        const method = head.method;

        // Route: GET /feed
        if (method == .get and std.mem.eql(u8, path, "/feed")) {
            self.db.feed(&self.resp_buf) catch {
                self.sendError(slot, c, 500, "internal server error", keep_alive);
                return;
            };
            self.sendResponse(slot, c, 200, self.resp_buf.slice(), keep_alive);
            return;
        }

        // Route: /posts/...
        if (std.mem.startsWith(u8, path, "/posts")) {
            const rem = path[6..];

            // POST /posts (create)
            if (rem.len == 0 and method == .post) {
                const claims = self.authenticate(head.auth) catch |err| {
                    self.sendAuthError(slot, c, err, keep_alive);
                    return;
                };

                // Parse body JSON: {"body":"..."}
                const jp = @import("jp.zig");
                var f: [1]jp.Field = undefined;
                jp.parseObject(body, &self.scratch, &.{"body"}, &f) catch {
                    self.sendError(slot, c, 400, "malformed JSON body", keep_alive);
                    return;
                };

                if (f[0].kind != .string) {
                    self.sendError(slot, c, 400, "body is required", keep_alive);
                    return;
                }

                const trimmed = trimJs(f[0].s);
                if (trimmed.len == 0) {
                    self.sendError(slot, c, 400, "body is required", keep_alive);
                    return;
                }

                const chars = std.unicode.utf8CountCodepoints(trimmed) catch {
                    self.sendError(slot, c, 400, "body is required", keep_alive);
                    return;
                };
                if (chars > 500) {
                    self.sendError(slot, c, 400, "body must be at most 500 characters", keep_alive);
                    return;
                }

                self.db.createPost(claims.uid, claims.name, trimmed, &self.resp_buf) catch {
                    self.sendError(slot, c, 500, "internal server error", keep_alive);
                    return;
                };
                self.sendResponse(slot, c, 201, self.resp_buf.slice(), keep_alive);
                return;
            }

            // Path starts with /posts/
            if (rem.len > 1 and rem[0] == '/') {
                const subpath = rem[1..];

                // Like: /posts/:id/like
                if (std.mem.endsWith(u8, subpath, "/like")) {
                    const id_str = subpath[0 .. subpath.len - 5];
                    const claims = self.authenticate(head.auth) catch |err| {
                        self.sendAuthError(slot, c, err, keep_alive);
                        return;
                    };

                    const post_id = parseId(id_str) orelse {
                        self.sendError(slot, c, 400, "invalid post id", keep_alive);
                        return;
                    };

                    if (method == .post) {
                        switch (self.db.likePost(claims.uid, post_id, &self.resp_buf)) {
                            .created => self.sendResponse(slot, c, 201, self.resp_buf.slice(), keep_alive),
                            .already_liked => self.sendResponse(slot, c, 200, self.resp_buf.slice(), keep_alive),
                            .not_found => self.sendError(slot, c, 404, "post not found", keep_alive),
                            .err => self.sendError(slot, c, 500, "internal server error", keep_alive),
                        }
                        return;
                    } else if (method == .delete) {
                        switch (self.db.unlikePost(claims.uid, post_id, &self.resp_buf)) {
                            .ok => self.sendResponse(slot, c, 200, self.resp_buf.slice(), keep_alive),
                            .not_found => self.sendError(slot, c, 404, "post not found", keep_alive),
                            .err => self.sendError(slot, c, 500, "internal server error", keep_alive),
                        }
                        return;
                    }
                }

                // Single post: GET /posts/:id
                if (method == .get) {
                    const post_id = parseId(subpath) orelse {
                        self.sendError(slot, c, 400, "invalid post id", keep_alive);
                        return;
                    };

                    switch (self.db.getPost(post_id, &self.resp_buf)) {
                        .ok => self.sendResponse(slot, c, 200, self.resp_buf.slice(), keep_alive),
                        .not_found => self.sendError(slot, c, 404, "post not found", keep_alive),
                        .err => self.sendError(slot, c, 500, "internal server error", keep_alive),
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
            self.sendResponse(slot, c, if (ok) 200 else 503, self.resp_buf.slice(), keep_alive);
            return;
        }

        self.sendError(slot, c, 404, "not found", keep_alive);
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

    fn sendAuthError(self: *Server, slot: u32, c: *Conn, err: AuthError, keep_alive: bool) void {
        switch (err) {
            error.MissingToken => self.sendError(slot, c, 401, "missing bearer token", keep_alive),
            error.InvalidToken => self.sendError(slot, c, 401, "invalid or expired token", keep_alive),
            error.InvalidPayload => self.sendError(slot, c, 401, "invalid token payload", keep_alive),
        }
    }

    fn sendError(self: *Server, slot: u32, c: *Conn, status: u16, msg: []const u8, keep_alive: bool) void {
        self.resp_buf.reset();
        self.resp_buf.writeAll("{\"error\":\"");
        self.resp_buf.writeJsonString(msg);
        self.resp_buf.writeAll("\"}");
        self.sendResponse(slot, c, status, self.resp_buf.slice(), keep_alive);
    }

    fn sendResponse(self: *Server, slot: u32, c: *Conn, status: u16, body: []const u8, keep_alive: bool) void {
        self.head_buf.reset();
        const status_line = switch (status) {
            200 => "HTTP/1.1 200 OK\r\n",
            201 => "HTTP/1.1 201 Created\r\n",
            400 => "HTTP/1.1 400 Bad Request\r\n",
            401 => "HTTP/1.1 401 Unauthorized\r\n",
            404 => "HTTP/1.1 404 Not Found\r\n",
            503 => "HTTP/1.1 503 Service Unavailable\r\n",
            else => "HTTP/1.1 500 Internal Server Error\r\n",
        };
        self.head_buf.writeAll(status_line);
        self.head_buf.writeAll("Content-Type: application/json\r\n");
        if (!keep_alive) {
            self.head_buf.writeAll("Connection: close\r\n");
        }
        self.head_buf.writeAll("Content-Length: ");
        self.head_buf.writeU64(body.len);
        self.head_buf.writeAll("\r\n\r\n");

        const total_len = self.head_buf.len + body.len;
        const out_mem = gpa.alloc(u8, total_len) catch return;
        @memcpy(out_mem[0..self.head_buf.len], self.head_buf.slice());
        @memcpy(out_mem[self.head_buf.len..][0..body.len], body);

        c.out = out_mem;
        c.out_len = total_len;
        c.out_off = 0;

        self.armSend(slot, c);
    }

    fn armSend(self: *Server, slot: u32, c: *Conn) void {
        const s = self.ring.get_sqe() catch return;
        const slice = c.out.?[c.out_off..c.out_len];
        s.prep_send(@intCast(slot), slice, 0);
        s.flags |= linux.IOSQE_FIXED_FILE;
        s.user_data = ud(.send, c.gen, slot);
        c.send_live = true;
    }

    fn onSend(self: *Server, slot: u32, gen: u32, res: i32) void {
        if (slot >= self.slots) return;
        const c = &self.conns[slot];
        if (!c.open or c.gen != gen) return;

        c.send_live = false;
        if (res <= 0) {
            self.closeConn(slot, c);
            return;
        }

        c.out_off += @intCast(res);
        if (c.out_off < c.out_len) {
            self.armSend(slot, c);
            return;
        }

        // Response fully sent
        if (c.out) |m| {
            gpa.free(m);
            c.out = null;
        }
        c.out_len = 0;
        c.out_off = 0;

        if (c.closing) {
            self.closeConn(slot, c);
        }
    }

    fn onShut(self: *Server, slot: u32, gen: u32) void {
        if (slot >= self.slots) return;
        const c = &self.conns[slot];
        if (c.gen != gen) return;
        c.shut_live = false;
        self.maybeFinishClose(slot, c);
    }

    fn closeConn(self: *Server, slot: u32, c: *Conn) void {
        if (!c.open or c.closing) return;
        c.closing = true;

        if (c.out) |m| {
            gpa.free(m);
            c.out = null;
        }
        if (c.pend_buf) |p| {
            gpa.free(p);
            c.pend_buf = null;
        }
        c.pend_len = 0;

        if (c.recv_live or c.send_live) {
            const s = self.ring.get_sqe() catch {
                self.maybeFinishClose(slot, c);
                return;
            };
            s.prep_shutdown(@intCast(slot), linux.SHUT.RDWR);
            s.flags |= linux.IOSQE_FIXED_FILE;
            s.user_data = ud(.shut, c.gen, slot);
            c.shut_live = true;
        }
        self.maybeFinishClose(slot, c);
    }

    fn maybeFinishClose(self: *Server, slot: u32, c: *Conn) void {
        if (!c.closing or c.recv_live or c.send_live or c.shut_live) return;
        c.closing = false;
        c.open = false;
        const s = self.ring.get_sqe() catch return;
        s.prep_close_direct(slot);
        s.user_data = ud(.close, 0, 0);
    }
};
