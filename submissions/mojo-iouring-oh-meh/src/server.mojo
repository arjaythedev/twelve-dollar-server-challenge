# The server: one thread, one event loop over several io_uring rings.
#
# A batch is one wake-up: reap every ring, answer reads straight from the
# batch's SQLite transaction, queue writes, run them, COMMIT, then submit all
# sends. No response leaves before its batch commits.
from std.ffi import c_int, external_call
from bytes import Bytes, balloc, bcopy, bat, eqlit, lit, Out, parse_pos_int, alloc_u32, alloc_int, alloc_u8, leak_string
from uring import *
from resp import Arenas, Resp, const_resp, MAX_RESP
from db import Db, CREATED_LEN
from http import Req, parse_request, P_COMPLETE, P_INCOMPLETE, P_BAD, M_GET, M_POST
from jwt import Jwt, JWT_OK, JWT_INVALID, JWT_PAYLOAD
from jsonscan import validate_body, BODY_OK, BODY_MALFORMED, BODY_REQUIRED, BODY_TOO_LONG

# user_data layout: op(4) | ring(4) | gen(24) | slot(32)
comptime OP_ACCEPT: UInt64 = 1
comptime OP_RECV: UInt64 = 2
comptime OP_SEND: UInt64 = 3
comptime OP_CLOSE: UInt64 = 4

comptime SQ_ENTRIES = 4096
comptime NBUFS = 2048
comptime BUFSIZE = 2048
comptime CQ_BATCH = 4096
comptime MAX_RINGS = 16
comptime TARGET_CONNS = 1_000_000
comptime PARTIAL_MAX = 16384
comptime SCRATCH_CHUNK = 1 << 20
comptime ACCEPT_DEPTH = 64

# connection flags
comptime F_OPEN: UInt8 = 1
comptime F_SENDING: UInt8 = 2
comptime F_CLOSE_AFTER: UInt8 = 4
comptime F_CLOSING: UInt8 = 8
comptime F_NEED_RECV: UInt8 = 32


@always_inline
def mk_ud(op: UInt64, ring: Int, gen: UInt32, slot: Int) -> UInt64:
    return (op << 60) | (UInt64(ring) << 56) | (UInt64(gen & 0xFFFFFF) << 32) | UInt64(slot)


@fieldwise_init
struct WriteReq(ImplicitlyCopyable, Copyable, Movable):
    var conn: Int
    var kind: Int          # 1 = post, 2 = like
    var user_id: Int64
    var post_id: Int64
    var name: Int          # address in scratch
    var name_len: Int
    var body: Int
    var body_len: Int
    var result: Int        # like: 0/1/2/-1; post: 1 ok / 0 fail
    var created: Int       # address of 24 bytes in scratch (post)


# Per-batch bump allocator for bodies and usernames that outlive the receive buffer.
struct Scratch:
    var chunks: List[Int]
    var cur: Int
    var used: Int

    def __init__(out self):
        self.chunks = List[Int]()
        self.chunks.append(Int(balloc(SCRATCH_CHUNK)))
        self.cur = 0
        self.used = 0

    def reset(mut self):
        self.cur = 0
        self.used = 0

    def take(mut self, n: Int) -> Bytes:
        if self.used + n > SCRATCH_CHUNK:
            self.cur += 1
            if self.cur == len(self.chunks):
                self.chunks.append(Int(balloc(SCRATCH_CHUNK)))
            self.used = 0
        var p = Bytes(unsafe_from_address=self.chunks[self.cur] + self.used)
        self.used += n
        return p


struct Server:
    var db: Db
    var jwt: Jwt
    var arenas: Arenas
    var scratch: Scratch
    var table: Int
    var nrings: Int          # rings opened so far
    var max_rings: Int
    var accept_ring: Int
    var accept_armed: Bool
    var accept_paused: Bool
    var accept_inflight: Int
    var ring_live: Pointer[Int, MutUntrackedOrigin]
    var ring_inflight: Pointer[Int, MutUntrackedOrigin]
    var lfd: Int
    var start_ms: Int
    var now: Int64
    # connection state, indexed by ring * table + slot
    var flags: Bytes
    var gen: Pointer[UInt32, MutUntrackedOrigin]
    var partial: Pointer[Int, MutUntrackedOrigin]     # malloc'd buffer address or 0
    var plen: Pointer[Int, MutUntrackedOrigin]
    var send_ptr: Pointer[Int, MutUntrackedOrigin]
    var send_len: Pointer[Int, MutUntrackedOrigin]
    var send_arena: Pointer[Int, MutUntrackedOrigin]
    var q_head: Pointer[Int, MutUntrackedOrigin]   # per connection: node index + 1, 0 = empty
    var q_tail: Pointer[Int, MutUntrackedOrigin]
    var qn_ptr: List[Int]      # node pool
    var qn_len: List[Int]
    var qn_arena: List[Int]
    var qn_next: List[Int]     # node index + 1, 0 = none
    var qn_free: List[Int]
    var cqes: Pointer[Cqe, MutUntrackedOrigin]
    var writes: List[WriteReq]
    var rearm: List[Int]
    var body_buf: Bytes
    var req: Req
    # constant responses
    var r_not_found: Resp
    var r_bad_id: Resp
    var r_post_missing: Resp
    var r_missing_token: Resp
    var r_bad_token: Resp
    var r_bad_payload: Resp
    var r_bad_json: Resp
    var r_body_required: Resp
    var r_body_long: Resp
    var r_internal: Resp
    var r_bad_request: Resp
    var r_too_large: Resp
    var r_degraded: Resp
    # stats
    var n_requests: Int
    var n_conns: Int
    var last_tick: Int

    def __init__(out self, db_path: String, secret: String, host: String, port: Int) raises:
        self.db = Db(db_path)
        self.db.startup(db_path)
        self.jwt = Jwt(leak_string(secret), secret.byte_length())
        self.arenas = Arenas()
        self.scratch = Scratch()
        self.table = table_size()
        # Test-only knob: a tiny table makes rings fill after a few connections so the
        # multi-ring path can be exercised locally. Never set on the benchmark box.
        var override = getenv("MOJO_TABLE_OVERRIDE", "")
        if override != "":
            self.table = Int(override)
        if self.table <= 0:
            raise Error("cannot determine fd limit")
        self.max_rings = (TARGET_CONNS + self.table - 1) // self.table
        if self.max_rings > MAX_RINGS:
            self.max_rings = MAX_RINGS
        if self.max_rings < 1:
            self.max_rings = 1
        var rc = ring_init(SQ_ENTRIES, self.table, NBUFS, BUFSIZE)
        if rc < 0:
            raise Error("ring init failed")
        self.nrings = 0
        self.accept_ring = 0
        self.accept_armed = False
        self.accept_paused = False
        self.accept_inflight = 0
        self.ring_live = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(MAX_RINGS * 8)))
        self.ring_inflight = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(MAX_RINGS * 8)))
        self.lfd = listen(host, port, 4096)
        if self.lfd < 0:
            raise Error("listen failed: " + String(self.lfd))
        self.start_ms = now_ms()
        self.now = unix_time()
        var n = self.max_rings * self.table
        self.flags = calloc_bytes(n)
        self.gen = Pointer[UInt32, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 4)))
        self.partial = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.plen = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.send_ptr = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.send_len = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.send_arena = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.q_head = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.q_tail = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(n * 8)))
        self.qn_ptr = List[Int]()
        self.qn_len = List[Int]()
        self.qn_arena = List[Int]()
        self.qn_next = List[Int]()
        self.qn_free = List[Int]()
        self.cqes = Pointer[Cqe, MutUntrackedOrigin](unsafe_from_address=Int(calloc_bytes(CQ_BATCH * 16)))
        self.writes = List[WriteReq]()
        self.rearm = List[Int]()
        self.body_buf = balloc(PARTIAL_MAX * 4)
        self.req = Req(self.body_buf)
        self.r_not_found = const_resp(404, '{"error":"not found"}')
        self.r_bad_id = const_resp(400, '{"error":"invalid post id"}')
        self.r_post_missing = const_resp(404, '{"error":"post not found"}')
        self.r_missing_token = const_resp(401, '{"error":"missing bearer token"}')
        self.r_bad_token = const_resp(401, '{"error":"invalid or expired token"}')
        self.r_bad_payload = const_resp(401, '{"error":"invalid token payload"}')
        self.r_bad_json = const_resp(400, '{"error":"malformed JSON body"}')
        self.r_body_required = const_resp(400, '{"error":"body is required"}')
        self.r_body_long = const_resp(400, '{"error":"body must be at most 500 characters"}')
        self.r_internal = const_resp(500, '{"error":"internal server error"}')
        self.r_bad_request = const_resp(400, '{"error":"bad request"}')
        self.r_too_large = const_resp(413, '{"error":"request too large"}')
        self.r_degraded = const_resp(503, '{"status":"degraded","db":"unreachable","error":"SELECT 1 failed"}')
        self.n_requests = 0
        self.n_conns = 0
        self.last_tick = self.start_ms
        if ring_open(0) != 0:
            raise Error("ring 0 open failed")
        self.nrings = 1
        self.pump_accepts()

    # ---- SQE helpers (retry once after flushing a full SQ)

    # Keep up to ACCEPT_DEPTH one-shot accepts outstanding, never more than the
    # accept ring has free slots; move to another ring when it is full.
    def pump_accepts(mut self):
        if self.accept_paused:
            return
        while self.accept_inflight < ACCEPT_DEPTH:
            var r = self.accept_ring
            var free = self.table - self.ring_live[unsafe_offset=r] - self.ring_inflight[unsafe_offset=r]
            if free <= 0:
                if not self.next_accept_ring():
                    log("all rings full; accepting paused")
                    self.accept_paused = True
                    return
                continue
            var want = ACCEPT_DEPTH - self.accept_inflight
            if want > free:
                want = free
            for _k in range(want):
                if prep_accept(r, self.lfd, mk_ud(OP_ACCEPT, r, 0, 0)) != 0:
                    _ = submit(r)
                    if prep_accept(r, self.lfd, mk_ud(OP_ACCEPT, r, 0, 0)) != 0:
                        return
                self.accept_inflight += 1
                self.ring_inflight[unsafe_offset=r] += 1
            self.accept_armed = True

    @always_inline
    def arm_recv(mut self, ring: Int, slot: Int, conn: Int):
        var ud = mk_ud(OP_RECV, ring, self.gen[unsafe_offset=conn], slot)
        if prep_recv(ring, slot, ud) != 0:
            _ = submit(ring)
            _ = prep_recv(ring, slot, ud)

    def start_send(mut self, ring: Int, slot: Int, conn: Int, ptr: Int, n: Int, arena: Int):
        self.send_ptr[unsafe_offset=conn] = ptr
        self.send_len[unsafe_offset=conn] = n
        self.send_arena[unsafe_offset=conn] = arena
        self.flags[unsafe_offset=conn] |= F_SENDING
        var ud = mk_ud(OP_SEND, ring, self.gen[unsafe_offset=conn], slot)
        if prep_send(ring, slot, ptr, n, ud) != 0:
            _ = submit(ring)
            _ = prep_send(ring, slot, ptr, n, ud)

    def do_close(mut self, ring: Int, slot: Int, conn: Int):
        var f = self.flags[unsafe_offset=conn]
        if (f & F_CLOSING) != 0:
            return
        self.flags[unsafe_offset=conn] = f | F_CLOSING
        var ud = mk_ud(OP_CLOSE, ring, self.gen[unsafe_offset=conn], slot)
        if prep_close(ring, slot, ud) != 0:
            _ = submit(ring)
            _ = prep_close(ring, slot, ud)

    # Queue a response for a connection: send now, or append behind the sends in flight
    # (pipelined requests are answered in order, one send at a time).
    def respond(mut self, ring: Int, slot: Int, conn: Int, var r: Resp, close: Bool):
        var f = self.flags[unsafe_offset=conn]
        if close:
            f |= F_CLOSE_AFTER
            self.flags[unsafe_offset=conn] = f
        if (f & F_SENDING) == 0:
            self.start_send(ring, slot, conn, r.ptr, r.len, r.arena)
            return
        var node: Int
        if len(self.qn_free) > 0:
            node = self.qn_free.pop()
            self.qn_ptr[node] = r.ptr
            self.qn_len[node] = r.len
            self.qn_arena[node] = r.arena
            self.qn_next[node] = 0
        else:
            node = len(self.qn_ptr)
            self.qn_ptr.append(r.ptr)
            self.qn_len.append(r.len)
            self.qn_arena.append(r.arena)
            self.qn_next.append(0)
        var tail = self.q_tail[unsafe_offset=conn]
        if tail == 0:
            self.q_head[unsafe_offset=conn] = node + 1
        else:
            self.qn_next[tail - 1] = node + 1
        self.q_tail[unsafe_offset=conn] = node + 1

    # Pop the next queued response for conn and start sending it; False if none.
    def send_next(mut self, ring: Int, slot: Int, conn: Int) -> Bool:
        var head = self.q_head[unsafe_offset=conn]
        if head == 0:
            return False
        var node = head - 1
        var nxt = self.qn_next[node]
        self.q_head[unsafe_offset=conn] = nxt
        if nxt == 0:
            self.q_tail[unsafe_offset=conn] = 0
        self.start_send(ring, slot, conn, self.qn_ptr[node], self.qn_len[node], self.qn_arena[node])
        self.qn_free.append(node)
        return True

    # Drop everything queued for a connection that is going away.
    def drop_queue(mut self, conn: Int):
        var head = self.q_head[unsafe_offset=conn]
        while head != 0:
            var node = head - 1
            self.arenas.release(self.qn_arena[node])
            head = self.qn_next[node]
            self.qn_free.append(node)
        self.q_head[unsafe_offset=conn] = 0
        self.q_tail[unsafe_offset=conn] = 0

    # ---- request handling

    def handle_request(mut self, ring: Int, slot: Int, conn: Int) raises:
        self.n_requests += 1
        var close = self.req.close
        var p = self.req.path
        var n = self.req.path_len
        if self.req.method == M_GET:
            if eqlit(p, n, "/feed"):
                var o = self.arenas.begin(MAX_RESP)
                self.db.write_feed(o)
                self.respond(ring, slot, conn, self.arenas.finish(200, o), close)
                return
            if n > 7 and eqlit(p, 7, "/posts/"):
                var id = parse_pos_int(p.unsafe_offset(7), n - 7)
                if id <= 0:
                    self.respond(ring, slot, conn, self.r_bad_id, close)
                    return
                var o = self.arenas.begin(MAX_RESP)
                if self.db.write_post(id, o):
                    self.respond(ring, slot, conn, self.arenas.finish(200, o), close)
                else:
                    self.respond(ring, slot, conn, self.r_post_missing, close)
                return
            if eqlit(p, n, "/health"):
                if self.db.health_ok():
                    var o = self.arenas.begin(128)
                    o.put_lit('{"status":"ok","db":"ok","uptime_s":')
                    o.put_int(Int64((now_ms() - self.start_ms) // 1000))
                    o.put(0x7D)
                    self.respond(ring, slot, conn, self.arenas.finish(200, o), close)
                else:
                    self.respond(ring, slot, conn, self.r_degraded, close)
                return
            self.respond(ring, slot, conn, self.r_not_found, close)
            return
        if self.req.method == M_POST:
            var is_create = eqlit(p, n, "/posts")
            var is_like = n > 12 and eqlit(p, 7, "/posts/") and eqlit(p.unsafe_offset(n - 5), 5, "/like")
            if not is_create and not is_like:
                self.respond(ring, slot, conn, self.r_not_found, close)
                return
            # auth first
            if not self.req.bearer:
                self.respond(ring, slot, conn, self.r_missing_token, close)
                return
            var rc = self.jwt.verify(self.req.tok, self.req.tok_len, self.now)
            if rc == JWT_INVALID:
                self.respond(ring, slot, conn, self.r_bad_token, close)
                return
            if rc == JWT_PAYLOAD:
                self.respond(ring, slot, conn, self.r_bad_payload, close)
                return
            var name = self.scratch.take(self.jwt.name_len)
            bcopy(name, self.jwt.name_ptr, self.jwt.name_len)
            if is_like:
                var id = parse_pos_int(p.unsafe_offset(7), n - 12)
                if id <= 0:
                    self.respond(ring, slot, conn, self.r_bad_id, close)
                    return
                self.writes.append(WriteReq(conn, 2, self.jwt.user_id, id, Int(name), self.jwt.name_len, 0, 0, 0, 0))
                if close:
                    self.flags[unsafe_offset=conn] |= F_CLOSE_AFTER
                return
            # create: validate the JSON body now, copy the trimmed body to scratch
            var blen = 0
            var brc = validate_body(self.req.body, self.req.body_len, self.body_buf, PARTIAL_MAX * 4, blen)
            if brc == BODY_MALFORMED:
                self.respond(ring, slot, conn, self.r_bad_json, close)
                return
            if brc == BODY_REQUIRED:
                self.respond(ring, slot, conn, self.r_body_required, close)
                return
            if brc == BODY_TOO_LONG:
                self.respond(ring, slot, conn, self.r_body_long, close)
                return
            var body = self.scratch.take(blen)
            bcopy(body, self.body_buf, blen)
            self.writes.append(WriteReq(conn, 1, self.jwt.user_id, 0, Int(name), self.jwt.name_len, Int(body), blen, 0, 0))
            if close:
                self.flags[unsafe_offset=conn] |= F_CLOSE_AFTER
            return
        self.respond(ring, slot, conn, self.r_not_found, close)

    # Parse as many complete requests as `data` holds; returns bytes consumed, or -1 to close.
    def consume(mut self, ring: Int, slot: Int, conn: Int, data: Bytes, n: Int) raises -> Int:
        var off = 0
        while off < n:
            parse_request(data.unsafe_offset(off), n - off, self.req)
            if self.req.status == P_INCOMPLETE:
                break
            if self.req.status == P_BAD:
                self.respond(ring, slot, conn, self.r_bad_request, True)
                return -1
            self.handle_request(ring, slot, conn)
            off += self.req.consumed
            if (self.flags[unsafe_offset=conn] & F_CLOSE_AFTER) != 0:
                return -1
        return off

    def on_recv(mut self, ring: Int, slot: Int, conn: Int, res: Int, cflags: UInt32) raises:
        var f = self.flags[unsafe_offset=conn]
        var has_buf = (cflags & CQE_F_BUFFER) != 0
        var bid = Int(cflags >> CQE_BUFFER_SHIFT)
        if (f & F_OPEN) == 0 or (f & F_CLOSING) != 0:
            if has_buf:
                buf_return(ring, bid)
            return
        if res <= 0:
            if has_buf:
                buf_return(ring, bid)
            if res == ENOBUFS:
                self.rearm.append(conn)
                return
            # EOF or error: close once any send in flight is done
            if (f & F_SENDING) != 0:
                self.flags[unsafe_offset=conn] = f | F_CLOSE_AFTER
            else:
                self.do_close(ring, slot, conn)
            return
        var data = buf_base(ring).unsafe_offset(bid * BUFSIZE)
        var n = res
        var pbuf = self.partial[unsafe_offset=conn]
        var consumed = 0
        if pbuf != 0:
            # continue a request that spans receives
            var have = self.plen[unsafe_offset=conn]
            if have + n > PARTIAL_MAX:
                buf_return(ring, bid)
                self.respond(ring, slot, conn, self.r_too_large, True)
                return
            bcopy(bat(pbuf + have), data, n)
            have += n
            self.plen[unsafe_offset=conn] = have
            consumed = self.consume(ring, slot, conn, bat(pbuf), have)
            buf_return(ring, bid)
            if consumed < 0:
                return
            if consumed == have:
                free_addr(pbuf)
                self.partial[unsafe_offset=conn] = 0
                self.plen[unsafe_offset=conn] = 0
            elif consumed > 0:
                var rest = have - consumed
                bcopy(bat(pbuf), bat(pbuf + consumed), rest)
                self.plen[unsafe_offset=conn] = rest
        else:
            consumed = self.consume(ring, slot, conn, data, n)
            if consumed < 0:
                buf_return(ring, bid)
                return
            if consumed < n:
                var rest = n - consumed
                var nb = malloc_bytes(PARTIAL_MAX)
                bcopy(bat(nb), data.unsafe_offset(consumed), rest)
                self.partial[unsafe_offset=conn] = nb
                self.plen[unsafe_offset=conn] = rest
            buf_return(ring, bid)
        if (cflags & CQE_F_MORE) == 0:
            self.rearm.append(conn)

    def on_send(mut self, ring: Int, slot: Int, conn: Int, res: Int):
        var f = self.flags[unsafe_offset=conn]
        if (f & F_OPEN) == 0:
            return
        if res < 0:
            self.arenas.release(self.send_arena[unsafe_offset=conn])
            self.flags[unsafe_offset=conn] = f & ~F_SENDING
            self.drop_queue(conn)
            self.do_close(ring, slot, conn)
            return
        var left = self.send_len[unsafe_offset=conn] - res
        if left > 0:
            var ptr = self.send_ptr[unsafe_offset=conn] + res
            self.send_ptr[unsafe_offset=conn] = ptr
            self.send_len[unsafe_offset=conn] = left
            var ud = mk_ud(OP_SEND, ring, self.gen[unsafe_offset=conn], slot)
            if prep_send(ring, slot, ptr, left, ud) != 0:
                _ = submit(ring)
                _ = prep_send(ring, slot, ptr, left, ud)
            return
        self.arenas.release(self.send_arena[unsafe_offset=conn])
        f &= ~F_SENDING
        self.flags[unsafe_offset=conn] = f
        if self.send_next(ring, slot, conn):
            return
        if (f & F_CLOSE_AFTER) != 0:
            self.do_close(ring, slot, conn)

    # Pick the ring to accept on: the next one in order, else any with free slots.
    def next_accept_ring(mut self) -> Bool:
        var nr = self.accept_ring + 1
        if nr < self.max_rings:
            if nr >= self.nrings:
                if ring_open(nr) != 0:
                    log("ring open failed; accepting paused")
                    return False
                self.nrings = nr + 1
            self.accept_ring = nr
            return True
        var best = -1
        var best_free = 0
        for r in range(self.nrings):
            var free = self.table - self.ring_live[unsafe_offset=r] - self.ring_inflight[unsafe_offset=r]
            if free > best_free:
                best = r
                best_free = free
        if best < 0:
            return False
        self.accept_ring = best
        return True

    def on_accept(mut self, ring: Int, res: Int, cflags: UInt32):
        self.accept_inflight -= 1
        self.ring_inflight[unsafe_offset=ring] -= 1
        if res >= 0:
            var slot = res
            var conn = ring * self.table + slot
            self.flags[unsafe_offset=conn] = F_OPEN
            self.gen[unsafe_offset=conn] += 1
            self.n_conns += 1
            self.ring_live[unsafe_offset=ring] += 1
            self.arm_recv(ring, slot, conn)
        elif res != EAGAIN:
            log("accept failed: " + String(res) + " on ring " + String(ring))

    def on_close(mut self, conn: Int):
        self.drop_queue(conn)
        self.flags[unsafe_offset=conn] = 0
        self.gen[unsafe_offset=conn] += 1
        self.n_conns -= 1
        self.ring_live[unsafe_offset=conn // self.table] -= 1
        var pbuf = self.partial[unsafe_offset=conn]
        if pbuf != 0:
            free_addr(pbuf)
            self.partial[unsafe_offset=conn] = 0
            self.plen[unsafe_offset=conn] = 0

    # ---- the write phase

    def run_writes(mut self) raises:
        self.db.close_cursors()
        for i in range(len(self.writes)):
            var w = self.writes[i]
            if w.kind == 1:
                var created = self.scratch.take(CREATED_LEN)
                var id: Int64 = 0
                var ok = self.db.insert_post(w.user_id, bat(w.body), w.body_len, id, created)
                self.writes[i].result = 1 if ok else 0
                self.writes[i].post_id = id
                self.writes[i].created = Int(created)
            else:
                self.writes[i].result = self.db.like(w.user_id, w.post_id)
        var committed = self.db.commit()
        for i in range(len(self.writes)):
            var w = self.writes[i]
            var conn = w.conn
            var ring = conn // self.table
            var slot = conn % self.table
            var close = (self.flags[unsafe_offset=conn] & F_CLOSE_AFTER) != 0
            if (self.flags[unsafe_offset=conn] & F_OPEN) == 0:
                continue
            if not committed or w.result < 0 or (w.kind == 1 and w.result == 0):
                self.respond(ring, slot, conn, self.r_internal, close)
                continue
            if w.kind == 2:
                if w.result == 0:
                    self.respond(ring, slot, conn, self.r_post_missing, close)
                    continue
                var o = self.arenas.begin(128)
                o.put_lit('{"liked":true,"already_liked":')
                if w.result == 2:
                    o.put_lit("true")
                else:
                    o.put_lit("false")
                o.put_lit(',"post_id":')
                o.put_int(w.post_id)
                o.put(0x7D)
                self.respond(ring, slot, conn, self.arenas.finish(201 if w.result == 1 else 200, o), close)
                continue
            var o = self.arenas.begin(MAX_RESP)
            o.put_lit('{"post":{"id":')
            o.put_int(w.post_id)
            o.put_lit(',"body":"')
            o.put_json(bat(w.body), w.body_len)
            o.put_lit('","created_at":"')
            o.put_bytes(bat(w.created), CREATED_LEN)
            o.put_lit('","author":"')
            o.put_json(bat(w.name), w.name_len)
            o.put_lit('","like_count":0}}')
            self.respond(ring, slot, conn, self.arenas.finish(201, o), close)
        self.writes.clear()

    # ---- the loop

    def tick(mut self):
        var t = now_ms()
        if t - self.last_tick < 1000:
            return
        self.last_tick = t
        if self.accept_paused:
            # resume when memory has freed up and some ring has room
            var kb = mem_available_kb()
            if (kb == 0 or kb > 256 * 1024) and self.next_accept_ring():
                self.accept_paused = False
                self.pump_accepts()
        else:
            var kb = mem_available_kb()
            if kb != 0 and kb < 160 * 1024:
                log("MemAvailable low; accepting paused")
                self.accept_paused = True

    def run(mut self) raises:
        for r in range(self.nrings):
            _ = submit(r)
        while True:
            if self.nrings == 1 and not self.accept_paused:
                _ = wait_one(0)
            else:
                _ = wait(1000)
            self.now = unix_time()
            self.scratch.reset()
            for r in range(self.nrings):
                while True:
                    var n = reap(r, self.cqes, CQ_BATCH)
                    for i in range(n):
                        var c = self.cqes[unsafe_offset=i]
                        var ud = c.user_data
                        var op = ud >> 60
                        var ring = Int((ud >> 56) & 0xF)
                        var g = UInt32((ud >> 32) & 0xFFFFFF)
                        var slot = Int(ud & 0xFFFFFFFF)
                        var res = Int(c.res)
                        if op == OP_ACCEPT:
                            self.on_accept(ring, res, c.flags)
                            continue
                        var conn = ring * self.table + slot
                        if (self.gen[unsafe_offset=conn] & 0xFFFFFF) != g:
                            if op == OP_RECV and (c.flags & CQE_F_BUFFER) != 0:
                                buf_return(ring, Int(c.flags >> CQE_BUFFER_SHIFT))
                            continue
                        if op == OP_RECV:
                            self.on_recv(ring, slot, conn, res, c.flags)
                        elif op == OP_SEND:
                            self.on_send(ring, slot, conn, res)
                        elif op == OP_CLOSE:
                            self.on_close(conn)
                    if n < CQ_BATCH:
                        break
            if len(self.writes) > 0:
                self.run_writes()
            else:
                _ = self.db.commit()
            # re-arm receives that ran out of buffers or ended their multishot
            if len(self.rearm) > 0:
                for i in range(len(self.rearm)):
                    var conn = self.rearm[i]
                    var f = self.flags[unsafe_offset=conn]
                    if (f & F_OPEN) != 0 and (f & F_CLOSING) == 0:
                        self.arm_recv(conn // self.table, conn % self.table, conn)
                self.rearm.clear()
            self.pump_accepts()
            for r in range(self.nrings):
                _ = buf_flush(r)
                _ = submit(r)
            self.tick()


def main() raises:
    var db_path = getenv("SQLITE_PATH", "")
    if db_path == "":
        raise Error("SQLITE_PATH is required")
    var secret = getenv("JWT_SECRET", "twelve-dollar-challenge")
    var host = getenv("HOST", "127.0.0.1")
    var port = Int(getenv("PORT", "3000"))
    var srv = Server(db_path, secret, host, port)
    var ck = db_path
    _ = external_call["sh_checkpointer_start", c_int](ck.as_c_string_span(), c_int(250))
    log("mojo server listening on " + host + ":" + String(port) + " (fd table " + String(srv.table) + " x up to " + String(srv.max_rings) + " rings, tail plan " + ("on" if srv.db.tail_ok else "off") + ")")
    srv.run()
