# The server: one thread, one event loop over several io_uring rings.
#
# A batch is one wake-up: answer what earlier passes parked, reap every ring,
# run bounded reads for connections whose multishot receive ended, answer reads
# straight from the batch's SQLite transaction, queue writes, run them, COMMIT,
# then submit all sends. No write's response leaves before its batch commits.
#
# After a stall every connection has a request waiting and the ring's provided
# buffers run out at once; the receives that lost end with ENOBUFS. Those
# connections are not re-armed together (that only repeats the shortage, pass
# after pass, while the requests sit in 3-4 KB kernel socket buffers). Each goes
# on its ring's read queue and is read with one single-shot, non-waiting receive
# per pass, never more at once than the ring has buffers; a short read or EAGAIN
# means the socket is drained and the multishot receive is armed again. A pass
# reads up to READ_MAX requests but answers at most ANSWER_MAX, parking the rest
# in the connection's own buffer (a copy the size of the request) on a ready
# list that later passes serve oldest first, so sockets drain as fast as reads
# can run and replies go out while the backlog is still being read.
#
# Convention: opaque C handles (ring, db, cursor) travel as Int addresses, which
# is ABI-identical to void* and sidesteps origin typing for memory Mojo never owns.
from std.ffi import c_int, external_call
from bytes import Bytes, balloc, bcopy, bmove, bat, eqlit, Out, parse_pos_int, calloc_t, leak_string
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
comptime OP_SHUTDOWN: UInt64 = 5   # the shutdown linked before a close; its completion is ignored
comptime OP_READ: UInt64 = 6       # a bounded single-shot read from the read queue
comptime MAX_RINGS = 16

comptime SQ_ENTRIES = 4096
comptime NBUFS = 2048
comptime BUFSIZE = 2048
comptime CQ_BATCH = 4096
comptime TARGET_CONNS = 1_000_000
comptime PARTIAL_MIN = 512
comptime PARTIAL_MAX = 16384      # one incomplete request may grow to this (413 past it)
comptime PARKED_MAX = 262144      # complete requests parked by the answer cap may grow to this
comptime SCRATCH_CHUNK = 1 << 20
# Accepts in flight bound the accept rate (one is re-armed only after a pass sees it
# complete); 64 fell behind a 2,000-connection/s ramp once passes slowed at the memory
# wall, the listen queue overflowed and the rest waited out SYN retransmits. Eight
# SO_REUSEPORT listeners give eight listen queues (net.core.somaxconn caps each at
# 4,096) to hold a ramp or a slow pass; each listener keeps its own share of the
# accepts in flight, re-armed where a completion came from, so a listener with a
# queue is never left waiting for its turn.
comptime ACCEPT_DEPTH = 512
comptime LISTEN_BACKLOG = 4096
comptime LISTENERS = 8
comptime CKPT_INTERVAL_MS = 250
# Drain first: a pass reads up to READ_MAX times (over as many rounds as the
# buffers allow) but answers at most ANSWER_MAX requests; the rest wait parked.
comptime READ_MAX = 16384
comptime ANSWER_MAX = 2048
# Batch formation: after a pass that handled at least BUSY_MIN completions, the
# next wait asks for BATCH_WANT completions or BATCH_US, so one wake-up (and its
# BEGIN/COMMIT and submits) is shared by more requests. Costs up to BATCH_US of
# latency only while the server is busy.
comptime BUSY_MIN = 8
comptime BATCH_WANT = 32
comptime BATCH_US = 250
# While writes are held for a WAL restart the wait is bounded, so the hold ends
# (backfilled, or expired) even if no completion arrives to wake the loop.
comptime HOLD_POLL_US = 2000

# connection flags
comptime F_OPEN: UInt8 = 1
comptime F_SENDING: UInt8 = 2
comptime F_CLOSE_AFTER: UInt8 = 4
comptime F_CLOSING: UInt8 = 8
comptime F_QUEUED: UInt8 = 16     # on its ring's read queue (no receive armed)
comptime F_READY: UInt8 = 32      # a parsed-but-unanswered request waits in `partial`
comptime F_DRAINED: UInt8 = 64    # read dry by a bounded read; multishot receive re-armed after the flush
comptime F_EOF: UInt8 = 128       # the peer's EOF (or a receive error) was seen: close once nothing is left to answer

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
    var mark: Int32         # last compaction that kept an entry for this slot (dedup)


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
    var node: Int          # the response-queue placeholder reserved at parse time
    var gen: UInt32        # the connection's generation then (slot reuse check)


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
    var lfds: List[Int]
    var accept_inflight_l: Pointer[Int, MutUntrackedOrigin]   # per listener
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
    var close_retry: List[Int]   # connections whose close could not get its two SQEs
    # Read queues, one circular buffer per ring, and the ready list, one for all
    # rings. Entries are hints: the connection's flag (F_QUEUED, F_READY) is the
    # truth, checked when the entry is taken, so an entry left behind by a close is
    # skipped. A connection is on a list at most once while its flag is set; the
    # buffers hold twice the table for the stale entries a close can leave.
    var rq: Pointer[Int, MutUntrackedOrigin]
    var rq_cap: Int
    var rq_head: Pointer[Int, MutUntrackedOrigin]
    var rq_n: Pointer[Int, MutUntrackedOrigin]
    var rd: Pointer[Int, MutUntrackedOrigin]
    var rd_cap: Int
    var rd_head: Int
    var rd_n: Int
    var nbufs: Int
    var held: Pointer[Int, MutUntrackedOrigin]   # per ring: buffers handed to us and not yet given back
    var drained: List[Int]   # read dry by a bounded read; their multishot receive is re-armed after the flush
    var answered: Int        # requests answered in this pass
    var capped: Bool         # the last consume stopped at ANSWER_MAX with data left
    var n_enobufs: Int
    var n_reads: Int
    var n_parked: Int
    var rq_peak: Int
    var rd_peak: Int
    var trace: Bool
    var stamp: Int32         # compaction counter, written to Conn.mark
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
    var last_tick_prev: Int
    var efd0_ready: Bool

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
        # Test-only knob: fewer provided buffers per ring, so a few hundred connections
        # can run the ring dry (ENOBUFS, the read queue) locally.
        self.nbufs = NBUFS
        var nb_override = getenv("MOJO_NBUFS_OVERRIDE", "")
        if nb_override != "":
            self.nbufs = Int(nb_override)
        self.max_rings = (TARGET_CONNS + self.table - 1) // self.table
        if self.max_rings > MAX_RINGS:
            self.max_rings = MAX_RINGS
        if self.max_rings < 1:
            self.max_rings = 1
        if ring_init(SQ_ENTRIES, self.table, self.nbufs, BUFSIZE) < 0:
            raise Error("ring init failed")
        self.nrings = 0
        self.accept_ring = 0
        self.accept_paused = False
        self.accept_inflight = 0
        self.ring_live = calloc_t[Int](MAX_RINGS)
        self.ring_inflight = calloc_t[Int](MAX_RINGS)
        self.lfds = List[Int]()
        self.accept_inflight_l = calloc_t[Int](LISTENERS)
        for _k in range(LISTENERS):
            var lfd = listen(host, port, LISTEN_BACKLOG)
            if lfd < 0:
                if len(self.lfds) > 0:
                    break   # the port allows fewer sharers: serve with what opened
                raise Error("listen failed: " + String(lfd))
            self.lfds.append(lfd)
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
        self.close_retry = List[Int]()
        self.rq_cap = 2 * self.table
        self.rq = calloc_t[Int](self.max_rings * self.rq_cap)
        self.rq_head = calloc_t[Int](MAX_RINGS)
        self.rq_n = calloc_t[Int](MAX_RINGS)
        self.rd_cap = 2 * self.max_rings * self.table
        self.rd = calloc_t[Int](self.rd_cap)
        self.rd_head = 0
        self.rd_n = 0
        self.held = calloc_t[Int](MAX_RINGS)
        self.drained = List[Int]()
        self.answered = 0
        self.capped = False
        self.n_enobufs = 0
        self.n_reads = 0
        self.n_parked = 0
        self.rq_peak = 0
        self.rd_peak = 0
        self.trace = getenv("MOJO_TRACE", "") != ""
        self.stamp = 0
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
        self.last_tick_prev = self.start_ms
        self.efd0_ready = False
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

    # Keep up to ACCEPT_DEPTH one-shot accepts outstanding, an equal share per
    # listener, never more than the accept ring has free slots; move to another
    # ring when it is full.
    def pump_accepts(mut self):
        if self.accept_paused:
            return
        var nl = len(self.lfds)
        # an accept in flight reserves a slot: with a test-sized table, keep the
        # reservation a fraction of the slots so connections can still be accepted
        var depth = ACCEPT_DEPTH
        if depth > self.table // 4:
            depth = self.table // 4
        var per = depth // nl
        if per < 1:
            per = 1
        for l in range(nl):
            while self.accept_inflight_l[unsafe_offset=l] < per:
                var r = self.accept_ring
                var free = self.ring_free(r)
                if free <= 0:
                    if not self.next_accept_ring():
                        log("all rings full; accepting paused")
                        self.accept_paused = True
                        return
                    continue
                var want = per - self.accept_inflight_l[unsafe_offset=l]
                if want > free:
                    want = free
                for _k in range(want):
                    if prep_accept(r, self.lfds[l], mk_ud(OP_ACCEPT, r, 0, l)) != 0:
                        return
                    self.accept_inflight += 1
                    self.accept_inflight_l[unsafe_offset=l] += 1
                    self.ring_inflight[unsafe_offset=r] += 1

    def on_accept(mut self, ring: Int, listener: Int, res: Int):
        self.accept_inflight -= 1
        self.accept_inflight_l[unsafe_offset=listener] -= 1
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

    # ---- the read queue and the ready list

    # Arm the multishot receive; if the submission queue has no room even after a
    # submit, the connection goes on the read queue instead, which keeps the retry
    # (a connection with no receive and on no list would hang).
    def arm_recv(mut self, ring: Int, slot: Int, conn: Int):
        if prep_recv(ring, slot, mk_ud(OP_RECV, ring, self.conns[unsafe_offset=conn].gen, slot)) != 0:
            self.queue_read(conn)

    # Compact a full queue: keep one entry per slot whose flag is set (the oldest),
    # drop the rest (entries left by closed lifetimes of the slot). Afterwards the
    # queue holds at most one entry per live slot, half its capacity, so an append
    # never overwrites. Rare: it needs more closes of queued connections than pops
    # between two passes.
    def compact_rq(mut self, r: Int):
        self.stamp += 1
        var base = r * self.rq_cap
        var h = self.rq_head[unsafe_offset=r]
        var n = self.rq_n[unsafe_offset=r]
        var kept = 0
        for i in range(n):
            var conn = self.rq[unsafe_offset=base + (h + i) % self.rq_cap]
            if (self.conns[unsafe_offset=conn].flags & F_QUEUED) != 0 and self.conns[unsafe_offset=conn].mark != self.stamp:
                self.conns[unsafe_offset=conn].mark = self.stamp
                self.rq[unsafe_offset=base + (h + kept) % self.rq_cap] = conn
                kept += 1
        self.rq_n[unsafe_offset=r] = kept

    def queue_read(mut self, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_QUEUED) != 0 or (f & F_OPEN) == 0 or (f & F_CLOSING) != 0:
            return
        var r = conn // self.table
        if self.rq_n[unsafe_offset=r] >= self.rq_cap:
            self.compact_rq(r)
        self.conns[unsafe_offset=conn].flags = f | F_QUEUED
        var n = self.rq_n[unsafe_offset=r]
        var at = (self.rq_head[unsafe_offset=r] + n) % self.rq_cap
        self.rq[unsafe_offset=r * self.rq_cap + at] = conn
        self.rq_n[unsafe_offset=r] = n + 1
        if n + 1 > self.rq_peak:
            self.rq_peak = n + 1

    def pop_read(mut self, r: Int) -> Int:
        var h = self.rq_head[unsafe_offset=r]
        var conn = self.rq[unsafe_offset=r * self.rq_cap + h]
        self.rq_head[unsafe_offset=r] = (h + 1) % self.rq_cap
        self.rq_n[unsafe_offset=r] -= 1
        return conn

    def compact_rd(mut self):
        self.stamp += 1
        var kept = 0
        for i in range(self.rd_n):
            var conn = self.rd[unsafe_offset=(self.rd_head + i) % self.rd_cap]
            if (self.conns[unsafe_offset=conn].flags & F_READY) != 0 and self.conns[unsafe_offset=conn].mark != self.stamp:
                self.conns[unsafe_offset=conn].mark = self.stamp
                self.rd[unsafe_offset=(self.rd_head + kept) % self.rd_cap] = conn
                kept += 1
        self.rd_n = kept

    def mark_ready(mut self, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_READY) != 0:
            return
        if self.rd_n >= self.rd_cap:
            self.compact_rd()
        self.conns[unsafe_offset=conn].flags = f | F_READY
        self.rd[unsafe_offset=(self.rd_head + self.rd_n) % self.rd_cap] = conn
        self.rd_n += 1
        self.n_parked += 1
        if self.rd_n > self.rd_peak:
            self.rd_peak = self.rd_n

    def mark_drained(mut self, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & (F_DRAINED | F_CLOSING | F_CLOSE_AFTER | F_EOF)) != 0:
            return
        self.conns[unsafe_offset=conn].flags = f | F_DRAINED
        self.drained.append(conn)

    def pending_work(self) -> Bool:
        if self.rd_n > 0 or len(self.drained) > 0:
            return True
        for r in range(self.nrings):
            if self.rq_n[unsafe_offset=r] > 0:
                return True
        return False

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
        var g = self.conns[unsafe_offset=conn].gen
        if prep_close(ring, slot, mk_ud(OP_SHUTDOWN, ring, g, slot), mk_ud(OP_CLOSE, ring, g, slot)) != 0:
            # no room even after a submit: try again at the end of the batch
            self.conns[unsafe_offset=conn].flags = f
            self.close_retry.append(conn)

    # Append a queue node for conn (ptr == 0 is a placeholder for a write whose
    # response is built after COMMIT). Returns the node index.
    def enqueue(mut self, conn: Int, ptr: Int, n: Int, arena: Int) -> Int:
        var node: Int
        if len(self.qn_free) > 0:
            node = self.qn_free.pop()
            self.qn_ptr[node] = ptr
            self.qn_len[node] = n
            self.qn_arena[node] = arena
            self.qn_next[node] = 0
        else:
            node = len(self.qn_ptr)
            self.qn_ptr.append(ptr)
            self.qn_len.append(n)
            self.qn_arena.append(arena)
            self.qn_next.append(0)
        var tail = Int(self.conns[unsafe_offset=conn].q_tail)
        if tail == 0:
            self.conns[unsafe_offset=conn].q_head = Int32(node + 1)
        else:
            self.qn_next[tail - 1] = node + 1
        self.conns[unsafe_offset=conn].q_tail = Int32(node + 1)
        return node

    # Queue a response for a connection: send now if nothing is ahead of it, else
    # append (pipelined requests are answered in order, one send in flight).
    def respond(mut self, ring: Int, slot: Int, conn: Int, var r: Resp, close: Bool):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_CLOSING) != 0:
            self.arenas.release(r.arena)   # a close is already on its way; nothing more goes out
            return
        if close:
            f |= F_CLOSE_AFTER
            self.conns[unsafe_offset=conn].flags = f
        if (f & F_SENDING) == 0 and self.conns[unsafe_offset=conn].q_head == 0:
            self.start_send(ring, slot, conn, r.ptr, r.len, r.arena)
            return
        _ = self.enqueue(conn, r.ptr, r.len, r.arena)

    # Start the next queued response; False if none, or if the head is a write still
    # waiting for its commit.
    def send_next(mut self, ring: Int, slot: Int, conn: Int) -> Bool:
        if (self.conns[unsafe_offset=conn].flags & F_CLOSING) != 0:
            self.drop_queue(conn)
            return False
        var head = Int(self.conns[unsafe_offset=conn].q_head)
        if head == 0:
            return False
        var node = head - 1
        if self.qn_ptr[node] == 0:
            return False
        var nxt = self.qn_next[node]
        self.conns[unsafe_offset=conn].q_head = Int32(nxt)
        if nxt == 0:
            self.conns[unsafe_offset=conn].q_tail = 0
        self.start_send(ring, slot, conn, self.qn_ptr[node], self.qn_len[node], self.qn_arena[node])
        self.qn_free.append(node)
        return True

    # Close a connection that is to close (a close request answered, or the peer's
    # EOF) once nothing is left: no send in flight, nothing queued, nothing parked.
    def close_if_done(mut self, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & (F_CLOSE_AFTER | F_EOF)) == 0 or (f & F_CLOSING) != 0 or (f & F_SENDING) != 0:
            return
        if self.conns[unsafe_offset=conn].q_head != 0:
            return
        if (f & F_READY) != 0 and (f & F_CLOSE_AFTER) == 0:
            return   # parked requests still to answer (EOF only: after a close request the rest is dropped)
        self.do_close(conn // self.table, conn % self.table, conn)

    # A placeholder got its response: send it if the connection is idle.
    def kick(mut self, conn: Int):
        var f = self.conns[unsafe_offset=conn].flags
        if (f & F_SENDING) == 0 and (f & F_CLOSING) == 0:
            if not self.send_next(conn // self.table, conn % self.table, conn):
                self.close_if_done(conn)

    # Drop everything queued for a connection that is going away. A placeholder
    # still waiting for its write is not freed: run_writes owns it until it has
    # resolved the write, and sees the -1 (orphan) mark then.
    def drop_queue(mut self, conn: Int):
        var head = Int(self.conns[unsafe_offset=conn].q_head)
        while head != 0:
            var node = head - 1
            head = self.qn_next[node]
            if self.qn_ptr[node] == 0:
                self.qn_ptr[node] = -1
            else:
                self.arenas.release(self.qn_arena[node])
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
                var node = self.enqueue(conn, 0, 0, -1)
                self.writes.append(WriteReq(conn, W_LIKE, self.jwt.user_id, id, 0, 0, 0, 0, 0, 0, node, self.conns[unsafe_offset=conn].gen))
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
            var node = self.enqueue(conn, 0, 0, -1)
            self.writes.append(WriteReq(conn, W_POST, self.jwt.user_id, 0, Int(name), self.jwt.name_len, Int(body), blen, 0, 0, node, self.conns[unsafe_offset=conn].gen))
            if close:
                self.conns[unsafe_offset=conn].flags |= F_CLOSE_AFTER
            return
        self.respond(ring, slot, conn, self.r_not_found, close)

    # Parse as many complete requests as `data` holds, up to the pass's answer cap;
    # returns bytes consumed, or -1 to close. `capped` says it stopped at the cap.
    def consume(mut self, ring: Int, slot: Int, conn: Int, data: Bytes, n: Int) raises -> Int:
        var off = 0
        self.capped = False
        while off < n:
            if self.answered >= ANSWER_MAX:
                self.capped = True
                break
            parse_request(data.unsafe_offset(off), n - off, self.req)
            if self.req.status == P_INCOMPLETE:
                break
            if self.req.status == P_BAD:
                self.respond(ring, slot, conn, self.r_bad_request, True)
                return -1
            self.handle_request(ring, slot, conn)
            self.answered += 1
            off += self.req.consumed
            if (self.conns[unsafe_offset=conn].flags & F_CLOSE_AFTER) != 0:
                return -1
        return off

    # Make the connection's partial-request buffer hold `need` bytes (realloc keeps
    # what is already there). False past the cap.
    # `tight`: size the buffer to the bytes (rounded to 64) rather than doubling from
    # 512: a parked complete request is read later as it is and never grows, and
    # after a stall there can be a parked request per connection.
    def grow_partial(mut self, conn: Int, need: Int, limit: Int, tight: Bool = False) -> Bool:
        if need > limit:
            return False
        var pbuf = self.conns[unsafe_offset=conn].partial
        if pbuf != 0 and Int(self.conns[unsafe_offset=conn].pcap) >= need:
            return True
        var want: Int
        if tight and pbuf == 0:
            want = (need + 63) & ~63
        else:
            want = PARTIAL_MIN
            while want < need:
                want *= 2
        if want > limit:
            want = limit
        pbuf = realloc_bytes(pbuf, want) if pbuf != 0 else malloc_bytes(want)
        self.conns[unsafe_offset=conn].partial = pbuf
        self.conns[unsafe_offset=conn].pcap = Int32(want)
        return True

    def free_partial(mut self, conn: Int):
        var pbuf = self.conns[unsafe_offset=conn].partial
        if pbuf != 0:
            free_addr(pbuf)
            self.conns[unsafe_offset=conn].partial = 0
            self.conns[unsafe_offset=conn].plen = 0
            self.conns[unsafe_offset=conn].pcap = 0

    # A terminal response (413) went out for a connection: whatever was parked is
    # dropped with it, so nothing is answered after or out of order with it.
    def too_large(mut self, ring: Int, slot: Int, conn: Int):
        self.conns[unsafe_offset=conn].flags &= ~F_READY
        self.respond(ring, slot, conn, self.r_too_large, True)

    # Received bytes for a connection (`data` is a provided buffer, given back here):
    # append to a partial request if one is pending, answer what is complete within
    # the cap, keep the rest. False if the connection is closing. While the cap
    # parks complete requests the buffer may grow to PARKED_MAX; one incomplete
    # request is still limited to PARTIAL_MAX.
    def ingest(mut self, ring: Int, slot: Int, conn: Int, data: Bytes, n: Int, bid: Int) raises -> Bool:
        var consumed: Int
        var rest: Int
        var f = self.conns[unsafe_offset=conn].flags
        var limit = PARKED_MAX if ((f & F_READY) != 0 or self.answered >= ANSWER_MAX) else PARTIAL_MAX
        var pbuf = self.conns[unsafe_offset=conn].partial
        if pbuf != 0:
            var have = Int(self.conns[unsafe_offset=conn].plen)
            if not self.grow_partial(conn, have + n, limit):
                buf_return(ring, bid)
                self.too_large(ring, slot, conn)
                return False
            pbuf = self.conns[unsafe_offset=conn].partial
            bcopy(bat(pbuf + have), data, n)
            buf_return(ring, bid)
            have += n
            self.conns[unsafe_offset=conn].plen = Int32(have)
            consumed = self.consume(ring, slot, conn, bat(pbuf), have)
            if consumed < 0:
                # a close request or a bad request: the rest of the input is dropped
                self.conns[unsafe_offset=conn].flags &= ~F_READY
                self.free_partial(conn)
                return False
            rest = have - consumed
            if rest == 0:
                self.free_partial(conn)
            elif consumed > 0:
                bmove(bat(pbuf), bat(pbuf + consumed), rest)
                self.conns[unsafe_offset=conn].plen = Int32(rest)
        else:
            consumed = self.consume(ring, slot, conn, data, n)
            if consumed < 0:
                buf_return(ring, bid)
                return False
            rest = n - consumed
            if rest > 0:
                if not self.grow_partial(conn, rest, limit, self.capped):
                    buf_return(ring, bid)
                    self.too_large(ring, slot, conn)
                    return False
                bcopy(bat(self.conns[unsafe_offset=conn].partial), data.unsafe_offset(consumed), rest)
                self.conns[unsafe_offset=conn].plen = Int32(rest)
            buf_return(ring, bid)
        if rest > 0:
            if self.capped:
                self.mark_ready(conn)
            elif rest > PARTIAL_MAX:
                # what is left is one incomplete request, past the single-request cap
                self.too_large(ring, slot, conn)
                return False
        return True

    # A receive completed: the multishot one (single=False) or a bounded read.
    def on_recv(mut self, ring: Int, slot: Int, conn: Int, res: Int, cflags: UInt32, single: Bool) raises:
        var f = self.conns[unsafe_offset=conn].flags
        var has_buf = (cflags & CQE_F_BUFFER) != 0
        var bid = Int(cflags >> CQE_BUFFER_SHIFT)
        if (f & F_OPEN) == 0 or (f & (F_CLOSING | F_CLOSE_AFTER)) != 0:
            # closed, closing, or closing once its responses are out: after a request
            # that asked to close, nothing more is read (it would replay what the
            # buffer still holds)
            if has_buf:
                buf_return(ring, bid)
            return
        if res <= 0:
            if has_buf:
                buf_return(ring, bid)
            if res == ENOBUFS:
                # the ring's buffers are out: read this one later, when they are back
                self.n_enobufs += 1
                self.queue_read(conn)
                return
            if single and res == EAGAIN:
                self.mark_drained(conn)   # back to a multishot receive, once the buffers are back
                return
            # EOF or error: everything parked or queued (including writes waiting for
            # their commit) is still answered first, then the connection closes
            self.conns[unsafe_offset=conn].flags = f | F_EOF
            self.close_if_done(conn)
            return
        var data = buf_base(ring).unsafe_offset(bid * BUFSIZE)
        if not self.ingest(ring, slot, conn, data, res, bid):
            return
        if single:
            # a full buffer may mean more is waiting; a shorter read means drained
            if res >= BUFSIZE:
                self.queue_read(conn)
            else:
                self.mark_drained(conn)
        elif (cflags & CQE_F_MORE) == 0:
            self.queue_read(conn)

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
        # not sent: either nothing is queued, or the head is a write still waiting
        # for its commit; only the first case may close
        self.close_if_done(conn)

    def on_close(mut self, conn: Int):
        self.drop_queue(conn)
        self.conns[unsafe_offset=conn].flags = 0
        self.conns[unsafe_offset=conn].gen += 1
        self.ring_live[unsafe_offset=conn // self.table] -= 1
        self.free_partial(conn)

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
            if self.qn_ptr[w.node] == -1:
                # the connection dropped its queue (close, send error) since the parse;
                # the write still ran, but nobody is waiting for the response
                self.qn_free.append(w.node)
                continue
            var r: Resp
            if not committed or w.result < 0 or (w.kind == W_POST and w.result == 0):
                r = self.r_internal
            elif w.kind == W_LIKE:
                if w.result == LIKE_MISSING:
                    r = self.r_post_missing
                else:
                    var o = self.arenas.begin(128)
                    o.put_lit('{"liked":true,"already_liked":')
                    if w.result == LIKE_DUP:
                        o.put_lit("true")
                    else:
                        o.put_lit("false")
                    o.put_lit(',"post_id":')
                    o.put_int(w.post_id)
                    o.put(0x7D)
                    r = self.arenas.finish(201 if w.result == LIKE_NEW else 200, o)
            else:
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
                r = self.arenas.finish(201, o)
            self.qn_ptr[w.node] = r.ptr
            self.qn_len[w.node] = r.len
            self.qn_arena[w.node] = r.arena
            self.kick(conn)
        self.writes.clear()

    # ---- the loop

    def reap_ring(mut self, r: Int) raises -> Int:
        var handled = 0
        while True:
            var n = reap(r, self.cqes, CQ_BATCH)
            handled += n
            for i in range(n):
                self.dispatch(self.cqes[unsafe_offset=i])
            if n < CQ_BATCH:
                break
        return handled

    def flush_bufs(mut self, r: Int):
        var n = buf_flush(r)
        if n > 0:
            self.held[unsafe_offset=r] -= n

    # Answer parked requests, oldest first, up to the cap.
    def serve_ready(mut self) raises:
        while self.rd_n > 0 and self.answered < ANSWER_MAX:
            var conn = self.rd[unsafe_offset=self.rd_head]
            self.rd_head = (self.rd_head + 1) % self.rd_cap
            self.rd_n -= 1
            var f = self.conns[unsafe_offset=conn].flags
            if (f & F_READY) == 0:
                continue
            self.conns[unsafe_offset=conn].flags = f & ~F_READY
            var pbuf = self.conns[unsafe_offset=conn].partial
            if (f & F_OPEN) == 0 or (f & F_CLOSING) != 0 or (f & F_CLOSE_AFTER) != 0 or pbuf == 0:
                continue
            var have = Int(self.conns[unsafe_offset=conn].plen)
            var consumed = self.consume(conn // self.table, conn % self.table, conn, bat(pbuf), have)
            if consumed < 0:
                self.free_partial(conn)   # a close or bad request: the rest of the input is dropped
                self.close_if_done(conn)
                continue
            var rest = have - consumed
            if rest == 0:
                self.free_partial(conn)
            else:
                if consumed > 0:
                    bmove(bat(pbuf), bat(pbuf + consumed), rest)
                    self.conns[unsafe_offset=conn].plen = Int32(rest)
                if self.capped:
                    self.mark_ready(conn)
                    continue
                if (f & F_EOF) != 0:
                    self.free_partial(conn)   # an incomplete request the peer will never finish
            self.close_if_done(conn)

    # Bounded reads for the read queues: per ring, as many single-shot receives as
    # it has free buffers, submitted and reaped in the same round, in rounds while
    # the queue and the pass's read budget last.
    def issue_reads(mut self) raises:
        var budget = READ_MAX
        for r in range(self.nrings):
            while self.rq_n[unsafe_offset=r] > 0 and budget > 0:
                var free = self.nbufs - self.held[unsafe_offset=r]
                if free <= 0:
                    break
                var want = self.rq_n[unsafe_offset=r]
                if want > free:
                    want = free
                if want > budget:
                    want = budget
                var issued = 0
                for _k in range(want):
                    var conn = self.pop_read(r)
                    var f = self.conns[unsafe_offset=conn].flags
                    if (f & F_QUEUED) == 0:
                        continue
                    self.conns[unsafe_offset=conn].flags = f & ~F_QUEUED
                    if (f & F_OPEN) == 0 or (f & F_CLOSING) != 0:
                        continue
                    var slot = conn % self.table
                    if prep_read(r, slot, mk_ud(OP_READ, r, self.conns[unsafe_offset=conn].gen, slot)) != 0:
                        self.queue_read(conn)   # no SQE even after a submit: next round
                        break
                    issued += 1
                if issued == 0:
                    break
                budget -= issued
                self.n_reads += issued
                var e0 = self.n_enobufs
                _ = submit(r)
                var got = self.reap_ring(r)
                self.flush_bufs(r)
                if self.trace:
                    log("trace: ring " + String(r) + " issued " + String(issued) + " free_before " + String(free) + " reaped " + String(got) + " enobufs+" + String(self.n_enobufs - e0) + " held_after " + String(self.held[unsafe_offset=r]) + " rq " + String(self.rq_n[unsafe_offset=r]) + " rd " + String(self.rd_n) + " answered " + String(self.answered))
                if self.n_enobufs > e0:
                    # multishot receives took buffers this pass had counted as free:
                    # nothing more can succeed on this ring until the next flush lands
                    break

    # Re-arm the multishot receive of every connection a bounded read drained. After
    # the flush, so the re-provided buffers precede these in the submission queue: a
    # multishot receive selects a buffer at submission, before it looks at the socket,
    # and with the group empty it would end with ENOBUFS at once. The flag guards
    # against a slot closed and accepted again since (its new connection was armed
    # by the accept).
    def arm_drained(mut self):
        for i in range(len(self.drained)):
            var conn = self.drained[i]
            var f = self.conns[unsafe_offset=conn].flags
            if (f & F_DRAINED) == 0:
                continue
            self.conns[unsafe_offset=conn].flags = f & ~F_DRAINED
            if (f & F_OPEN) != 0 and (f & F_CLOSING) == 0 and (f & F_QUEUED) == 0 and (f & F_EOF) == 0:
                self.arm_recv(conn // self.table, conn % self.table, conn)
        self.drained.clear()

    def dispatch(mut self, c: Cqe) raises:
        var ud = c.user_data
        var op = ud >> 60
        var ring = Int((ud >> 56) & 0xF)
        var g = UInt32((ud >> 32) & 0xFFFFFF)
        var slot = Int(ud & 0xFFFFFFFF)
        var res = Int(c.res)
        if op == OP_ACCEPT:
            self.on_accept(ring, slot, res)   # the slot field carries the listener index
            return
        if (c.flags & CQE_F_BUFFER) != 0:
            self.held[unsafe_offset=ring] += 1
        var conn = ring * self.table + slot
        if (self.conns[unsafe_offset=conn].gen & 0xFFFFFF) != g:
            # a completion for an earlier life of this slot
            if (c.flags & CQE_F_BUFFER) != 0:
                buf_return(ring, Int(c.flags >> CQE_BUFFER_SHIFT))
            return
        if op == OP_SHUTDOWN:
            return
        if op == OP_RECV:
            self.on_recv(ring, slot, conn, res, c.flags, False)
        elif op == OP_READ:
            self.on_recv(ring, slot, conn, res, c.flags, True)
        elif op == OP_SEND:
            self.on_send(ring, slot, conn, res)
        elif op == OP_CLOSE:
            self.on_close(conn)

    def tick(mut self):
        var t = now_ms()
        if t - self.last_tick < 1000:
            return
        self.last_tick = t
        # a line every minute on how the WAL has been kept short (restarts should do it all)
        if (t - self.start_ms) // 60000 != (self.last_tick_prev - self.start_ms) // 60000:
            log("wal: holds " + String(self.db.holds) + ", restarts " + String(self.db.restarts) + ", expired " + String(self.db.holds_expired) + ", loop truncates " + String(self.db.truncates))
            log("recv: enobufs " + String(self.n_enobufs) + ", bounded reads " + String(self.n_reads) + ", parked " + String(self.n_parked) + ", read queue peak " + String(self.rq_peak) + ", ready peak " + String(self.rd_peak) + "; MemAvailable " + String(mem_available_kb() // 1024) + " MB")
        self.last_tick_prev = t
        # (No accept pause on low MemAvailable any more: at the wall it only kept the
        # users the test was ramping waiting in the SYN queue until they timed out,
        # 4,200 of 120k on the rig with 1.1 GB pinned. A refused connection saves nothing.)
        if self.accept_paused and self.next_accept_ring():
            # paused because every ring was full: a ring has room again
            self.accept_paused = False
            self.pump_accepts()

    # One ring and nothing paused: submit and wait in a single enter. Otherwise:
    # submit each ring, then epoll on their eventfds.
    def single_mode(self) -> Bool:
        return self.nrings == 1 and self.single_wait_ok and not self.accept_paused

    def run(mut self) raises:
        var single = self.single_mode()
        var busy = False
        if not single:
            for r in range(self.nrings):
                _ = submit(r)
        while True:
            if self.pending_work():
                # parked requests or queued reads: don't sleep, just submit (and run
                # any deferred completion work) and go again
                if single:
                    _ = submit(0)
                else:
                    _ = wait(0)   # the end of the last pass submitted every ring
            elif single:
                var rc: Int
                if self.db.writes_held:
                    rc = wait_one_batch(0, 1, HOLD_POLL_US)
                elif busy:
                    rc = wait_one_batch(0, BATCH_WANT, BATCH_US)
                else:
                    rc = wait_one(0)
                if rc < 0 and rc != EAGAIN and rc != -4:
                    log("wait_one failed: " + String(rc) + "; using epoll waits")
                    self.single_wait_ok = False
            else:
                if not self.efd0_ready:
                    if ring_enable_efd(0) == 0:
                        self.efd0_ready = True
                    else:
                        log("eventfd registration failed; epoll waits will time out instead")
                _ = wait(HOLD_POLL_US // 1000 if self.db.writes_held else (100 if not self.efd0_ready else 1000))
                if busy:
                    settle(BATCH_US)
            self.now = unix_time()
            self.db.release_hold()
            # held writes keep their names and bodies in scratch across batches
            if len(self.writes) == 0:
                self.scratch.reset()
            self.answered = 0
            self.serve_ready()
            var e0 = self.n_enobufs
            var handled = 0
            for r in range(self.nrings):
                handled += self.reap_ring(r)
            if self.trace and (handled > 100 or self.n_enobufs > e0):
                log("trace: pass reaped " + String(handled) + " enobufs+" + String(self.n_enobufs - e0) + " held " + String(self.held[unsafe_offset=0]) + " rq " + String(self.rq_n[unsafe_offset=0]) + " rd " + String(self.rd_n) + " answered " + String(self.answered))
            busy = handled >= BUSY_MIN
            # hand buffers back, then read for the connections whose receive ran out of them
            for r in range(self.nrings):
                self.flush_bufs(r)
            self.issue_reads()
            self.arm_drained()
            if len(self.writes) > 0 and not self.db.writes_held:
                self.run_writes()
            else:
                _ = self.db.commit()
            if len(self.close_retry) > 0:
                var n = len(self.close_retry)
                for i in range(n):
                    var conn = self.close_retry[i]
                    if (self.conns[unsafe_offset=conn].flags & F_OPEN) != 0:
                        self.do_close(conn // self.table, conn % self.table, conn)
                # keep only what do_close re-queued during this pass
                var left = List[Int]()
                for i in range(n, len(self.close_retry)):
                    left.append(self.close_retry[i])
                self.close_retry = left^
            self.pump_accepts()
            self.tick()
            single = self.single_mode()
            if not single:
                for r in range(self.nrings):
                    _ = submit(r)


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
    var binary_ok: c_int = 0
    var locked = Int(external_call["sh_lock_code", Int](Pointer(to=binary_ok)))
    log("locked " + String(locked // 1024) + " KB of code and read-only data" + ("" if binary_ok != 0 else " (the binary itself only partly: RLIMIT_MEMLOCK)"))
    log("mojo server listening on " + host + ":" + String(port) + " (fd table " + String(srv.table) + " x up to " + String(srv.max_rings) + " rings, tail plan " + ("on" if srv.db.tail_ok else "off") + ")")
    srv.run()
