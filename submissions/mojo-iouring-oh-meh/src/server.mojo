# The server: one thread, one event loop over several io_uring rings.
#
# A batch is one wake-up: reap every ring, answer reads straight from the
# batch's SQLite transaction, queue writes, run them, COMMIT, then submit all
# sends. No response leaves before its batch commits.
#
# Convention: opaque C handles (ring, db, cursor) travel as Int addresses, which
# is ABI-identical to void* and sidesteps origin typing for memory Mojo never owns.
from std.ffi import c_int, external_call
from bytes import Bytes, balloc, bcopy, bat, eqlit, Out, parse_pos_int, calloc_t, leak_string
from uring import *
from resp import Arenas, Resp, const_resp, POST_MAX, FEED_MAX, CREATE_MAX
from db import Db, CREATED_LEN, LIKE_MISSING, LIKE_NEW, LIKE_DUP
from http import Req, parse_request, P_COMPLETE, P_INCOMPLETE, P_BAD, M_GET, M_POST
from jwt import Jwt, JWT_OK, JWT_INVALID, JWT_PAYLOAD
from jsonscan import validate_body, BODY_OK, BODY_MALFORMED, BODY_REQUIRED, BODY_TOO_LONG

# user_data layout: op(4) | ring(4) | gen(24) | slot(32). The 4-bit ring field is
# why MAX_RINGS is 16 here and in shim.c.
comptime OP_ACCEPT: UInt64 = 1
comptime OP_RECV: UInt64 = 2
comptime OP_SEND: UInt64 = 3
comptime OP_CLOSE: UInt64 = 4
comptime MAX_RINGS = 16

comptime SQ_ENTRIES = 4096
comptime NBUFS = 2048
comptime BUFSIZE = 2048
comptime CQ_BATCH = 4096
comptime TARGET_CONNS = 1_000_000
comptime PARTIAL_MIN = 512
comptime PARTIAL_MAX = 16384
comptime SCRATCH_CHUNK = 1 << 20
comptime ACCEPT_DEPTH = 64
comptime LISTEN_BACKLOG = 4096
comptime CKPT_INTERVAL_MS = 250
# Accepting pauses below MEM_PAUSE_KB of MemAvailable and resumes above
# MEM_RESUME_KB; the gap keeps it from flapping while the kernel reclaims.
comptime MEM_PAUSE_KB = 160 * 1024
comptime MEM_RESUME_KB = 256 * 1024

# connection flags
comptime F_OPEN: UInt8 = 1
comptime F_SENDING: UInt8 = 2
comptime F_CLOSE_AFTER: UInt8 = 4
comptime F_CLOSING: UInt8 = 8

# write kinds and like results
comptime W_POST = 1
comptime W_LIKE = 2


@always_inline
def mk_ud(op: UInt64, ring: Int, gen: UInt32, slot: Int) -> UInt64:
    return (op << 60) | (UInt64(ring) << 56) | (UInt64(gen & 0xFFFFFF) << 32) | UInt64(slot)


# Everything the loop knows about one connection, one cache line, in a
# calloc'ed table indexed by ring * table + slot (untouched slots cost nothing).
@fieldwise_init
struct Conn(ImplicitlyCopyable, Copyable, Movable):
    var send_ptr: Int
    var partial: Int        # malloc'd buffer address or 0
    var send_len: Int32
    var send_arena: Int32
    var plen: Int32
    var pcap: Int32
    var q_head: Int32       # node index + 1, 0 = empty
    var q_tail: Int32
    var gen: UInt32
    var flags: UInt8
    var _pad: Int64
    var _pad2: Int32


@fieldwise_init
struct WriteReq(ImplicitlyCopyable, Copyable, Movable):
    var conn: Int
    var kind: Int          # W_POST or W_LIKE
    var user_id: Int64
    var post_id: Int64
    var name: Int          # address in scratch (W_POST)
    var name_len: Int
    var body: Int
    var body_len: Int
    var result: Int        # W_LIKE: a LIKE_* code; W_POST: 1 ok / 0 fail
    var created: Int       # address of 24 bytes in scratch (W_POST)


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
    var accept_paused: Bool
    var accept_inflight: Int
    var ring_live: Pointer[Int, MutUntrackedOrigin]
    var ring_inflight: Pointer[Int, MutUntrackedOrigin]
    var lfd: Int
    var start_ms: Int
    var now: Int64
    var single_wait_ok: Bool
    var conns: Pointer[Conn, MutUntrackedOrigin]
    # response queue nodes (pipelined responses waiting behind a send)
    var qn_ptr: List[Int]
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
        if ring_init(SQ_ENTRIES, self.table, NBUFS, BUFSIZE) < 0:
            raise Error("ring init failed")
        self.nrings = 0
        self.accept_ring = 0
        self.accept_paused = False
        self.accept_inflight = 0
        self.ring_live = calloc_t[Int](MAX_RINGS)
        self.ring_inflight = calloc_t[Int](MAX_RINGS)
        self.lfd = listen(host, port, LISTEN_BACKLOG)
        if self.lfd < 0:
            raise Error("listen failed: " + String(self.lfd))
        self.start_ms = now_ms()
        self.now = unix_time()
        self.single_wait_ok = True
        self.conns = calloc_t[Conn](self.max_rings * self.table)
        self.qn_ptr = List[Int]()
        self.qn_len = List[Int]()
        self.qn_arena = List[Int]()
        self.qn_next = List[Int]()
        self.qn_free = List[Int]()
        self.cqes = calloc_t[Cqe](CQ_BATCH)
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
        self.last_tick = self.start_ms
        if ring_open(0) != 0:
            raise Error("ring 0 open failed")
        self.nrings = 1
        self.pump_accepts()

    # ---- accepting

    @always_inline
    def ring_free(self, r: Int) -> Int:
        return self.table - self.ring_live[unsafe_offset=r] - self.ring_inflight[unsafe_offset=r]

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
            var free = self.ring_free(r)
            if free > best_free:
                best = r
                best_free = free
        if best < 0:
            return False
        self.accept_ring = best
        return True

    # Keep up to ACCEPT_DEPTH one-shot accepts outstanding, never more than the
    # accept ring has free slots; move to another ring when it is full.
    def pump_accepts(mut self):
        if self.accept_paused:
            return
        while self.accept_inflight < ACCEPT_DEPTH:
            var r = self.accept_ring
            var free = self.ring_free(r)
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
                    return
                self.accept_inflight += 1
                self.ring_inflight[unsafe_offset=r] += 1

    def on_accept(mut self, ring: Int, res: Int):
        self.accept_inflight -= 1
        self.ring_inflight[unsafe_offset=ring] -= 1
        if res >= 0:
            var slot = res
            var conn = ring * self.table + slot
            self.conns[unsafe_offset=conn].flags = F_OPEN
            self.conns[unsafe_offset=conn].gen += 1
            self.ring_live[unsafe_offset=ring] += 1
            self.arm_recv(ring, slot, conn)
        elif res != EAGAIN:
            log("accept failed: " + String(res) + " on ring " + String(ring))

    # ---- sends and closes

    @always_inline
    def arm_recv(mut self, ring: Int, slot: Int, conn: Int):
        _ = prep_recv(ring, slot, mk_ud(OP_RECV, ring, self.conns[unsafe_offset=conn].gen, slot))

    def start_send(mut self, ring: Int, slot: Int, conn: Int, ptr: Int, n: Int, arena: Int):
        self.conns[unsafe_offset=conn].send_ptr = ptr
        self.conns[unsafe_offset=conn].send_len = Int32(n)
        self.conns[unsafe_offset=conn].send_arena = Int32(arena)
        self.conns[unsafe_offset=conn].flags |= F_SENDING
        _ = prep_send(ring, slot, ptr, n, mk_ud(OP_SEND, ring, self.conns[unsafe_offset=conn].gen, slot))

    def do_close(mut self, ring: Int, slot: Int, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_CLOSING) != 0:
            return
        self.conns[unsafe_offset=conn].flags = f | F_CLOSING
        _ = prep_close(ring, slot, mk_ud(OP_CLOSE, ring, self.conns[unsafe_offset=conn].gen, slot))

    # Queue a response for a connection: send now, or append behind the sends in flight
    # (pipelined requests are answered in order, one send at a time).
    def respond(mut self, ring: Int, slot: Int, conn: Int, var r: Resp, close: Bool):
        var f = self.conns[unsafe_offset=conn].flags
        if close:
            f |= F_CLOSE_AFTER
            self.conns[unsafe_offset=conn].flags = f
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
        var tail = Int(self.conns[unsafe_offset=conn].q_tail)
        if tail == 0:
            self.conns[unsafe_offset=conn].q_head = Int32(node + 1)
        else:
            self.qn_next[tail - 1] = node + 1
        self.conns[unsafe_offset=conn].q_tail = Int32(node + 1)

    # Pop the next queued response for conn and start sending it; False if none.
    def send_next(mut self, ring: Int, slot: Int, conn: Int) -> Bool:
        var head = Int(self.conns[unsafe_offset=conn].q_head)
        if head == 0:
            return False
        var node = head - 1
        var nxt = self.qn_next[node]
        self.conns[unsafe_offset=conn].q_head = Int32(nxt)
        if nxt == 0:
            self.conns[unsafe_offset=conn].q_tail = 0
        self.start_send(ring, slot, conn, self.qn_ptr[node], self.qn_len[node], self.qn_arena[node])
        self.qn_free.append(node)
        return True

    # Drop everything queued for a connection that is going away.
    def drop_queue(mut self, conn: Int):
        var head = Int(self.conns[unsafe_offset=conn].q_head)
        while head != 0:
            var node = head - 1
            self.arenas.release(self.qn_arena[node])
            head = self.qn_next[node]
            self.qn_free.append(node)
        self.conns[unsafe_offset=conn].q_head = 0
        self.conns[unsafe_offset=conn].q_tail = 0

    # ---- request handling

    def handle_request(mut self, ring: Int, slot: Int, conn: Int) raises:
        var close = self.req.close
        var p = self.req.path
        var n = self.req.path_len
        if self.req.method == M_GET:
            if eqlit(p, n, "/feed"):
                var o = self.arenas.begin(FEED_MAX)
                self.db.write_feed(o)
                self.respond(ring, slot, conn, self.arenas.finish(200, o), close)
                return
            if n > 7 and eqlit(p, 7, "/posts/"):
                var id = parse_pos_int(p.unsafe_offset(7), n - 7)
                if id <= 0:
                    self.respond(ring, slot, conn, self.r_bad_id, close)
                    return
                var o = self.arenas.begin(POST_MAX)
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
            if is_like:
                var id = parse_pos_int(p.unsafe_offset(7), n - 12)
                if id <= 0:
                    self.respond(ring, slot, conn, self.r_bad_id, close)
                    return
                self.writes.append(WriteReq(conn, W_LIKE, self.jwt.user_id, id, 0, 0, 0, 0, 0, 0))
                if close:
                    self.conns[unsafe_offset=conn].flags |= F_CLOSE_AFTER
                return
            # create: validate the JSON body now, copy the trimmed body and the
            # token's username to scratch (the receive buffer is returned after this)
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
            var name = self.scratch.take(self.jwt.name_len)
            bcopy(name, self.jwt.name_ptr, self.jwt.name_len)
            var body = self.scratch.take(blen)
            bcopy(body, self.body_buf, blen)
            self.writes.append(WriteReq(conn, W_POST, self.jwt.user_id, 0, Int(name), self.jwt.name_len, Int(body), blen, 0, 0))
            if close:
                self.conns[unsafe_offset=conn].flags |= F_CLOSE_AFTER
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
            if (self.conns[unsafe_offset=conn].flags & F_CLOSE_AFTER) != 0:
                return -1
        return off

    # Keep `rest` bytes of an incomplete request for the connection, sized to fit.
    def keep_partial(mut self, conn: Int, src: Bytes, rest: Int) -> Bool:
        var pbuf = self.conns[unsafe_offset=conn].partial
        var cap = Int(self.conns[unsafe_offset=conn].pcap)
        if rest > PARTIAL_MAX:
            return False
        if pbuf == 0 or cap < rest:
            var want = PARTIAL_MIN
            while want < rest:
                want *= 2
            if want > PARTIAL_MAX:
                want = PARTIAL_MAX
            pbuf = realloc_bytes(pbuf, want) if pbuf != 0 else malloc_bytes(want)
            self.conns[unsafe_offset=conn].partial = pbuf
            self.conns[unsafe_offset=conn].pcap = Int32(want)
        if Int(src) != pbuf:
            bcopy(bat(pbuf), src, rest)
        self.conns[unsafe_offset=conn].plen = Int32(rest)
        return True

    def on_recv(mut self, ring: Int, slot: Int, conn: Int, res: Int, cflags: UInt32) raises:
        var f = self.conns[unsafe_offset=conn].flags
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
                self.conns[unsafe_offset=conn].flags = f | F_CLOSE_AFTER
            else:
                self.do_close(ring, slot, conn)
            return
        var data = buf_base(ring).unsafe_offset(bid * BUFSIZE)
        var n = res
        var pbuf = self.conns[unsafe_offset=conn].partial
        var consumed = 0
        if pbuf != 0:
            # continue a request that spans receives: append, then parse the whole
            var have = Int(self.conns[unsafe_offset=conn].plen)
            if not self.keep_partial(conn, bat(pbuf), have + n):
                buf_return(ring, bid)
                self.respond(ring, slot, conn, self.r_too_large, True)
                return
            pbuf = self.conns[unsafe_offset=conn].partial
            bcopy(bat(pbuf + have), data, n)
            have += n
            consumed = self.consume(ring, slot, conn, bat(pbuf), have)
            buf_return(ring, bid)
            if consumed < 0:
                return
            if consumed == have:
                free_addr(pbuf)
                self.conns[unsafe_offset=conn].partial = 0
                self.conns[unsafe_offset=conn].plen = 0
                self.conns[unsafe_offset=conn].pcap = 0
            elif consumed > 0:
                var rest = have - consumed
                bcopy(bat(pbuf), bat(pbuf + consumed), rest)
                self.conns[unsafe_offset=conn].plen = Int32(rest)
        else:
            consumed = self.consume(ring, slot, conn, data, n)
            if consumed < 0:
                buf_return(ring, bid)
                return
            if consumed < n:
                if not self.keep_partial(conn, data.unsafe_offset(consumed), n - consumed):
                    buf_return(ring, bid)
                    self.respond(ring, slot, conn, self.r_too_large, True)
                    return
            buf_return(ring, bid)
        if (cflags & CQE_F_MORE) == 0:
            self.rearm.append(conn)

    def on_send(mut self, ring: Int, slot: Int, conn: Int, res: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_OPEN) == 0:
            return
        if res < 0:
            self.arenas.release(Int(self.conns[unsafe_offset=conn].send_arena))
            self.conns[unsafe_offset=conn].flags = f & ~F_SENDING
            self.drop_queue(conn)
            self.do_close(ring, slot, conn)
            return
        var left = Int(self.conns[unsafe_offset=conn].send_len) - res
        if left > 0:
            var ptr = self.conns[unsafe_offset=conn].send_ptr + res
            self.conns[unsafe_offset=conn].send_ptr = ptr
            self.conns[unsafe_offset=conn].send_len = Int32(left)
            _ = prep_send(ring, slot, ptr, left, mk_ud(OP_SEND, ring, self.conns[unsafe_offset=conn].gen, slot))
            return
        self.arenas.release(Int(self.conns[unsafe_offset=conn].send_arena))
        f &= ~F_SENDING
        self.conns[unsafe_offset=conn].flags = f
        if self.send_next(ring, slot, conn):
            return
        if (f & F_CLOSE_AFTER) != 0:
            self.do_close(ring, slot, conn)

    def on_close(mut self, conn: Int):
        self.drop_queue(conn)
        self.conns[unsafe_offset=conn].flags = 0
        self.conns[unsafe_offset=conn].gen += 1
        self.ring_live[unsafe_offset=conn // self.table] -= 1
        var pbuf = self.conns[unsafe_offset=conn].partial
        if pbuf != 0:
            free_addr(pbuf)
            self.conns[unsafe_offset=conn].partial = 0
            self.conns[unsafe_offset=conn].plen = 0
            self.conns[unsafe_offset=conn].pcap = 0

    # ---- the write phase

    def run_writes(mut self) raises:
        self.db.close_cursors()
        for i in range(len(self.writes)):
            var w = self.writes[i]
            if w.kind == W_POST:
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
            var f = self.conns[unsafe_offset=conn].flags
            var close = (f & F_CLOSE_AFTER) != 0
            if (f & F_OPEN) == 0:
                continue
            if not committed or w.result < 0 or (w.kind == W_POST and w.result == 0):
                self.respond(ring, slot, conn, self.r_internal, close)
                continue
            if w.kind == W_LIKE:
                if w.result == LIKE_MISSING:
                    self.respond(ring, slot, conn, self.r_post_missing, close)
                    continue
                var o = self.arenas.begin(128)
                o.put_lit('{"liked":true,"already_liked":')
                if w.result == LIKE_DUP:
                    o.put_lit("true")
                else:
                    o.put_lit("false")
                o.put_lit(',"post_id":')
                o.put_int(w.post_id)
                o.put(0x7D)
                self.respond(ring, slot, conn, self.arenas.finish(201 if w.result == LIKE_NEW else 200, o), close)
                continue
            var o = self.arenas.begin(CREATE_MAX)
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

    def dispatch(mut self, c: Cqe) raises:
        var ud = c.user_data
        var op = ud >> 60
        var ring = Int((ud >> 56) & 0xF)
        var g = UInt32((ud >> 32) & 0xFFFFFF)
        var slot = Int(ud & 0xFFFFFFFF)
        var res = Int(c.res)
        if op == OP_ACCEPT:
            self.on_accept(ring, res)
            return
        var conn = ring * self.table + slot
        if (self.conns[unsafe_offset=conn].gen & 0xFFFFFF) != g:
            # a completion for an earlier life of this slot
            if op == OP_RECV and (c.flags & CQE_F_BUFFER) != 0:
                buf_return(ring, Int(c.flags >> CQE_BUFFER_SHIFT))
            return
        if op == OP_RECV:
            self.on_recv(ring, slot, conn, res, c.flags)
        elif op == OP_SEND:
            self.on_send(ring, slot, conn, res)
        elif op == OP_CLOSE:
            self.on_close(conn)

    def tick(mut self):
        var t = now_ms()
        if t - self.last_tick < 1000:
            return
        self.last_tick = t
        var kb = mem_available_kb()
        if self.accept_paused:
            if (kb == 0 or kb > MEM_RESUME_KB) and self.next_accept_ring():
                self.accept_paused = False
                self.pump_accepts()
        elif kb != 0 and kb < MEM_PAUSE_KB:
            log("MemAvailable low; accepting paused")
            self.accept_paused = True

    def run(mut self) raises:
        var single = True
        while True:
            # Sleep. One ring: submit and wait in a single enter. Several rings:
            # submit each, then epoll on their eventfds.
            single = self.nrings == 1 and self.single_wait_ok and not self.accept_paused
            if single:
                var rc = wait_one(0)
                if rc < 0 and rc != EAGAIN and rc != -4:
                    log("wait_one failed: " + String(rc) + "; using epoll waits")
                    self.single_wait_ok = False
            else:
                _ = wait(1000)
            self.now = unix_time()
            self.scratch.reset()
            for r in range(self.nrings):
                while True:
                    var n = reap(r, self.cqes, CQ_BATCH)
                    for i in range(n):
                        self.dispatch(self.cqes[unsafe_offset=i])
                    if n < CQ_BATCH:
                        break
            if len(self.writes) > 0:
                self.run_writes()
            else:
                _ = self.db.commit()
            # hand buffers back before re-arming receives that ran out of them
            for r in range(self.nrings):
                _ = buf_flush(r)
            if len(self.rearm) > 0:
                for i in range(len(self.rearm)):
                    var conn = self.rearm[i]
                    var f = self.conns[unsafe_offset=conn].flags
                    if (f & F_OPEN) != 0 and (f & F_CLOSING) == 0:
                        self.arm_recv(conn // self.table, conn % self.table, conn)
                self.rearm.clear()
            self.pump_accepts()
            if not (self.nrings == 1 and self.single_wait_ok and not self.accept_paused):
                for r in range(self.nrings):
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
    _ = external_call["sh_checkpointer_start", c_int](ck.as_c_string_span(), c_int(CKPT_INTERVAL_MS))
    log("mojo server listening on " + host + ":" + String(port) + " (fd table " + String(srv.table) + " x up to " + String(srv.max_rings) + " rings, tail plan " + ("on" if srv.db.tail_ok else "off") + ")")
    srv.run()
