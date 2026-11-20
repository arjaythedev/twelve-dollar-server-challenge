// The $12 server, in Goose.
//
// One thread serves every connection through io_uring and every request through SQLite. All of
// the server is here: the event loop and its connection table, the accept policy and when to
// add a ring, group commit and the write queue, the checkpoint policy, HTTP/1.1, routing, HS256
// (SHA-256 and HMAC included), a validating JSON reader, the JSON writer, the body's Unicode
// trim, SQLite's record format, and the read plans over SQLite's B-tree cursors.
//
// C is what Goose cannot be: net.c is the kernel (io_uring, sockets, and the bytes the kernel
// reads and writes while an operation is in flight) and db.c is SQLite (cursor moves, row
// copies, the two INSERTs, the checkpoint thread). Neither decides anything.
import std;
import os;

// ---------------------------------------------------------------------------------------------
// C: the kernel (net.c)

struct Cqe { ud: u64, res: i32, flags: u32 }

extern fn net_listen(host: const u8[:], port: i64, n: i32) -> i32;
extern fn net_slot_limit() -> i64;
extern fn net_ring_add(slots: i64) -> i32;
extern fn net_enter(ring: i32, wait: i32, ns: i64);
extern fn net_sweep(ring: i32);
extern fn net_reap(ring: i32, out: Cqe[>..]&) -> i64;
extern fn net_accept(ring: i32, listener: i32, ud: u64);
extern fn net_recv(ring: i32, slot: i32, ud: u64);
extern fn net_send(ring: i32, slot: i32, ud: u64);
extern fn net_sent(ring: i32, slot: i32, n: i32, ud: u64) -> i32;
extern fn net_close(ring: i32, slot: i32, ud_shut: u64, ud_close: u64);
extern fn net_rx(ring: i32, bid: i32, len: i32, out: u8[>..]&);
extern fn net_rx_keep(ring: i32, slot: i32, bid: i32, len: i32) -> i64;
extern fn net_rx_drop(ring: i32, bid: i32);
extern fn net_keep(ring: i32, slot: i32, bytes: const u8[:]);
extern fn net_kept(ring: i32, slot: i32, out: u8[>..]&);
extern fn net_out(ring: i32, slot: i32, head: const u8[:], body: const u8[:]);
extern fn net_out_len(ring: i32, slot: i32) -> i64;
extern fn net_out_cut(ring: i32, slot: i32, len: i64);
extern fn net_free(ring: i32, slot: i32);
extern fn net_drop_output(ring: i32, slot: i32);
extern fn net_drop_kept(ring: i32, slot: i32);
extern fn net_lock_code() -> i64;

// C: SQLite (db.c)

struct FeedRow { id: i64, likes: i64, body_len: i64, created_len: i64, author_len: i64 }

extern fn db_open(path: const u8[:], err: u8[>..]&) -> i32;
extern fn db_begin() -> i32;
extern fn db_commit() -> bool;
extern fn db_seek(tree: i32, key: i64) -> i32;
extern fn db_seek_ge(tree: i32, key: i64) -> i32;
extern fn db_first(tree: i32) -> i32;
extern fn db_next(tree: i32) -> i32;
extern fn db_row(tree: i32, out: u8[>..]&) -> i64;
extern fn db_row_start(tree: i32, into: u8[24]&) -> i64;
extern fn db_add_post(user_id: i64, body: const u8[:], id: i64&, created: u8[>..]&) -> i32;
extern fn db_add_like(user_id: i64, post_id: i64) -> i32;
extern fn db_ping(err: u8[>..]&) -> i32;
extern fn db_feed_step(row: FeedRow&, text: u8[>..]&) -> i32;
extern fn db_feed_reset();
extern fn db_ck_ask();
extern fn db_ck_poll() -> i32;
extern fn db_wal_frames() -> i64;
extern fn db_wal_restarts();

// The trees db.c keeps a cursor on during a pass.
let POSTS: i32 = 0;
let USERS: i32 = 1;
let FEED: i32 = 2;     // posts_created_at_id_idx
let LIKES: i32 = 3;    // likes_post_id_idx

let FEED_LEN = 20;
// Like counts come from one walk of the likes index when the feed's ids span at most this many.
let LIKE_SPAN = 512;
let MAX_HEAD = 16384;
let MAX_BODY = 1048576;
let MAX_CHARS = 500;

// ---------------------------------------------------------------------------------------------
// Connections. A connection is (ring, slot): its slot in that ring's table of direct
// descriptors. Its bytes are in net.c; this table holds what the loop knows about it.

struct Conn {
    gen: u16,     // bumped when the slot is reused, so completions for an older connection drop
    flags: u16,
    mark: u32,    // pending output's length when this pass first touched the connection
    need: u32,    // kept input is a request still arriving: its full length once known, else 0
}

let F_OPEN: u16 = 1;
let F_CLOSE: u16 = 2;       // close once the output is out
let F_QUEUED: u16 = 4;      // a write of this connection waits in the write queue
let F_EOF: u16 = 8;         // the client finished sending while its write was queued
let F_CLOSING: u16 = 16;    // shutdown and close are on their way
let F_SENDING: u16 = 32;
let F_TOUCHED: u16 = 64;    // in this pass's touched list
let F_ANSWERED: u16 = 128;  // answered something in this pass
let F_KEPT: u16 = 256;      // net.c keeps input of this connection

struct RingState { open: i64, full: bool, accepts: i32[8] }

let LISTENERS = 8;
let MAX_RINGS = 16;

// user_data: operation (8 bits) | ring (8) | generation (16) | slot or listener (32)
let OP_ACCEPT: u64 = 1;
let OP_RECV: u64 = 2;
let OP_SEND: u64 = 3;
let OP_SHUT: u64 = 4;
let OP_CLOSE: u64 = 5;

let CQE_BUFFER: u32 = 1;    // IORING_CQE_F_BUFFER
let CQE_MORE: u32 = 2;      // IORING_CQE_F_MORE
let ENOBUFS: i32 = 105;
let ENFILE: i32 = 23;

var conns: Conn[>..];
var rs: RingState[16];
var nrings = 0;
var slots = 0;          // per ring
var depth = 0;          // accepts in flight on the accepting ring, over all listeners
var margin = 0;         // a ring stops accepting while fewer than this many slots are free
var acc_ring = 0;
var ring_retry = 0;     // adding a ring failed: not again before this (µs)

var cqes: Cqe[>..];
var data: u8[>..];      // the bytes of the connection being served
var touched: i64[>..];  // connections with output (or a close) for the end of this pass
var rearm: i64[>..];    // connections whose multishot receive ended
var err500: u8[>..];

// A pass waits on ring 0 for this many completions or this long, whichever comes first, so a
// busy pass shares one transaction across many requests.
var wait_min: i32 = 256;
var wait_ns = 1000000;

// ---------------------------------------------------------------------------------------------
// The write queue. A write is validated when it arrives and then waits here, with everything it
// needs, until the next write pass: all queued writes run in one transaction and share its
// COMMIT (group commit, rule 6), and the pages several of them touch reach the log once. Each
// response still waits for the COMMIT of its own write.

struct Job {
    ci: i64,
    gen: u16,
    like: bool,
    close: bool,           // the request asked to close the connection after its response
    uid: i64,
    post_id: i64,
    who: u8[varint],       // the token's username (a new post's author)
    text: u8[varint],      // a new post's trimmed body
}

var jobs: Job[>..];
var njobs = 0;
var write_window = 5000;   // µs a queued write waits for company; FEED_WRITE_US overrides it
var write_at = 0;          // µs; when the queue runs
var writes_now = false;    // a write pass is running: writes execute as they are parsed
var now_us = 0;            // the pass's clock

// Checkpoints: db.c's thread copies the log back when asked; the policy is here. Past ck_start
// log frames, ask for a copy. When it is done, hold the write queue (reads go on) and ask
// again: that copy takes the frames written meanwhile, and once the whole log is copied the
// next write starts the log over. Past ck_high the queue is held too, so the log stays bounded
// when the loop leaves the thread no idle time. A hold lasts at most GATE_US.
var ck_start = 16384;      // FEED_CK_FRAMES overrides it
var ck_high = 40960;
let GATE_US = 200000;
var ck_phase = 0;          // 0 idle, 1 copying, 2 copying the tail with writes held
var gate = false;
var gate_since = 0;

var stats_on = false;
var st_at = 0;
var st_passes = 0;
var st_reqs = 0;
var st_cqes = 0;
var st_writes = 0;
var st_wpasses = 0;
var st_nobuf = 0;
var st_rearms = 0;
var st_gates = 0;
var st_restarts = 0;
var st_maxpass = 0;
var st_serve_us = 0;
var st_write_us = 0;
var st_commit_us = 0;

// ---------------------------------------------------------------------------------------------
// Scratch, reused from request to request as storage only: each is cleared before use, and
// nothing read from the database outlives the request that read it.

var head: u8[>..];        // a response's status line and headers
var body: u8[>..];        // a response's body
var text: u8[>..];        // record bytes copied out of SQLite for the request being served
var rec: u8[>..];         // one index entry
var unesc: u8[>..];       // a decoded JSON string
var keybuf: u8[>..];      // a decoded JSON member name
var jwt_head: u8[>..];    // the token's decoded header
var jwt_body: u8[>..];    // and payload
var who: u8[>..];         // the verified token's username
var created: u8[>..];     // a new post's created_at

var hmac_inner: u32[8];   // SHA-256 state after the key's inner and outer pad blocks: the key
var hmac_outer: u32[8];   // schedule, computed once (not a verification result)
var b64: u8[256];
var started_us = 0;

// A feed row: in `text`, the body is b0..b1, created_at b1..c1 and the author a0..a1.
struct Row { id: i64, uid: i64, b0: i64, b1: i64, c1: i64, a0: i64, a1: i64, likes: i64 }
var rows: Row[20];
var order: i64[20];
var nrows = 0;
var like_counts: u32[513];

// ---------------------------------------------------------------------------------------------
// The loop

fn main() {
    if !startup() { exit(1); }
    loop {
        // Every other ring submits and collects without waiting; ring 0 goes last and waits.
        var r = nrings - 1;
        while r > 0 { net_sweep(r as! i32); r--; }
        net_enter(0, wait_min, wait_ns);
        now_us = clock_ns() / 1000;
        for k in nrings { drain(k); }
        let t1 = if stats_on { clock_ns() / 1000 } else { 0 };
        if njobs > 0 && !gate && now_us >= write_at { run_writes(); }
        let t2 = if stats_on { clock_ns() / 1000 } else { 0 };
        // No response of this pass reaches the kernel before its COMMIT returns.
        let committed = db_commit();
        if stats_on {
            let t3 = clock_ns() / 1000;
            st_serve_us = max(st_serve_us, t1 - now_us);
            st_write_us = max(st_write_us, t2 - t1);
            st_commit_us = max(st_commit_us, t3 - t2);
        }
        flush(committed);
        rearm_receives();
        accept_more();
        checkpoints();
        if stats_on { stats(); }
    }
}

fn log(s: const u8[:]) {
    var m: u8[>..] = [];
    m.append(s);
    m.push('\n');
    write_stderr(m);
}

fn env_int(name: const u8[:], lo: i64, hi: i64, into: i64&) {
    var v: u8[>..] = [];
    if !env(name, v) { return; }
    let n, ok = parse_int(v);
    if ok && n >= lo && n <= hi { into = n; }
}

fn startup() -> bool {
    var path: u8[>..] = [];
    var host: u8[>..] = [];
    var port_s: u8[>..] = [];
    var secret: u8[>..] = [];
    if !env("SQLITE_PATH", path) { path.append("feed.db"); }
    if !env("HOST", host) { host.append("127.0.0.1"); }
    if !env("PORT", port_s) { port_s.append("3000"); }
    env("JWT_SECRET", secret);
    let port, port_ok = parse_int(port_s);
    if !port_ok || port <= 0 || port > 65535 { log("PORT must be 1-65535"); return false; }
    // Test knobs; none is set on the benchmark box.
    env_int("FEED_WRITE_US", 0, 1000000, write_window);
    env_int("FEED_CK_FRAMES", 64, 1000000, ck_start);
    ck_high = ck_start + 24576;
    var w = wait_ns / 1000;
    env_int("FEED_WAIT_US", 50, 100000, w);
    wait_ns = w * 1000;
    var knob: u8[>..] = [];
    stats_on = env("FEED_STATS", knob);

    var err: u8[>..] = [];
    if db_open(path, err) != 0 {
        var m: u8[>..] = [];
        format(m, "database: ", err);
        log(m);
        return false;
    }
    for i in 256 { b64[i] = 0xff; }
    let alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for i in 64 { b64[alphabet[i]] = i as! u8; }
    hmac_key(secret);
    started_us = clock_ns() / 1000;
    err500.append("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n");
    err500.append("{\"error\":\"internal server error\"}");

    // Rings as large as the descriptor limit allows. A ring is left alone while fewer slots are
    // free than the accepts in flight plus some room: an accept that finds no free slot has
    // already taken the connection off the listen queue.
    slots = net_slot_limit();
    env_int("FEED_SLOTS", 64, 1048576, slots);
    depth = clamp(slots / 32, LISTENERS, 512) / LISTENERS * LISTENERS;
    margin = depth + 64;
    var m: u8[>..] = [];
    let lrc = net_listen(host, port, LISTENERS);
    if lrc != 0 {
        format(m, "listen ", host, ":", port, " failed (errno ", -lrc, ")");
        log(m);
        return false;
    }
    if !add_ring() { log("io_uring setup failed (needs Linux 6.1+ and io_uring allowed)"); return false; }
    let locked = net_lock_code();
    format(m, "listening on ", host, ":", port, "; io_uring, ", slots, " direct descriptors per ring, ",
        locked / 1024, " KB of code locked");
    log(m);
    return true;
}

fn add_ring() -> bool {
    if nrings >= MAX_RINGS { return false; }
    if net_ring_add(slots) < 0 { return false; }
    push_n(conns, Conn { gen: 0, flags: 0, mark: 0, need: 0 }, slots);
    rs[nrings] = RingState { open: 0, full: false, accepts: [0; 8] };
    nrings++;
    if nrings > 1 {
        var m: u8[>..] = [];
        format(m, "ring ", nrings - 1, " added");
        log(m);
    }
    return true;
}

inline fn ud(op: u64, ci: i64, gen: u16) -> u64 {
    return (op << 56) | (((ci / slots) as! u64) << 48) | ((gen as u64) << 32) | ((ci % slots) as! u64);
}

inline fn ring_of(ci: i64) -> i32 { return (ci / slots) as! i32; }
inline fn slot_of(ci: i64) -> i32 { return (ci % slots) as! i32; }

fn drain(r: i64) {
    cqes.clear();
    net_reap(r as! i32, cqes);
    st_cqes += cqes.len;
    for k in cqes.len {
        let e = cqes[k];
        let op = e.ud >> 56;
        let gen = ((e.ud >> 32) & 0xffff) as! u16;
        let low = (e.ud & 0xffffffff) as! i64;
        if op == OP_RECV { on_recv(r * slots + low, gen, e.res, e.flags); }
        else if op == OP_SEND { on_send(r * slots + low, gen, e.res); }
        else if op == OP_ACCEPT { on_accept(r, low, e.res); }
        else if op == OP_CLOSE { on_closed(r * slots + low, gen); }
    }
}

fn on_accept(r: i64, listener: i64, res: i32) {
    rs[r].accepts[listener] -= 1;
    if res >= 0 {
        let ci = r * slots + res;
        let c .= conns[ci];
        c.gen += 1;
        c.flags = F_OPEN;
        c.mark = 0;
        c.need = 0;
        net_recv(r as! i32, res, ud(OP_RECV, ci, c.gen));
        rs[r].open++;
        if rs[r].open + margin >= slots { rs[r].full = true; }
    } else if res == -ENFILE {
        rs[r].full = true;
    }
}

fn on_recv(ci: i64, gen: u16, res: i32, flags: u32) {
    let c .= conns[ci];
    let r = ring_of(ci);
    let has_buf = (flags & CQE_BUFFER) != 0;
    let bid = (flags >> 16) as! i32;
    if c.gen != gen || (c.flags & F_OPEN) == 0 || (c.flags & F_CLOSING) != 0 {
        if has_buf { net_rx_drop(r, bid); }
        return;
    }
    if res > 0 && has_buf {
        if (c.flags & F_CLOSE) != 0 {
            net_rx_drop(r, bid);   // its answers are done; what follows is dropped
        } else if (c.flags & F_QUEUED) != 0 {
            net_rx_keep(r, slot_of(ci), bid, res);   // behind a queued write: wait with it
            c.flags |= F_KEPT;
        } else if (c.flags & F_KEPT) != 0 {
            // More of a request that is still arriving; looked at again once it can be whole.
            let have = net_rx_keep(r, slot_of(ci), bid, res);
            if have >= c.need { serve_kept(ci); }
        } else {
            data.clear();
            net_rx(r, bid, res, data);
            serve(ci, data);
        }
        if (flags & CQE_MORE) == 0 { rearm.push(ci); }
    } else if res > 0 || res == -ENOBUFS {
        // The receive ended for lack of a buffer; armed again at the end of the pass, when the
        // buffers this pass used are back.
        st_nobuf++;
        rearm.push(ci);
    } else {
        // End of stream or an error: answer what was asked, then close.
        if has_buf { net_rx_drop(r, bid); }
        if (c.flags & F_QUEUED) != 0 { c.flags |= F_EOF; }
        else { close_after(ci); }
    }
}

fn on_send(ci: i64, gen: u16, res: i32) {
    let c .= conns[ci];
    if c.gen != gen || (c.flags & F_SENDING) == 0 { return; }
    if res < 0 {
        c.flags &= ~F_SENDING;
        net_drop_output(ring_of(ci), slot_of(ci));
        close_after(ci);
        return;
    }
    if net_sent(ring_of(ci), slot_of(ci), res, ud(OP_SEND, ci, gen)) != 0 { return; }
    c.flags &= ~F_SENDING;
    // Output that queued up behind this send goes out with the pass's other responses.
    if net_out_len(ring_of(ci), slot_of(ci)) > 0 || (c.flags & F_CLOSE) != 0 { touch(ci); }
}

fn on_closed(ci: i64, gen: u16) {
    let c .= conns[ci];
    let r = ci / slots;
    if c.gen == gen && (c.flags & F_OPEN) != 0 {
        net_free(ring_of(ci), slot_of(ci));
        c.flags = 0;
        rs[r].open--;
    }
    if rs[r].full && rs[r].open + 2 * margin < slots { rs[r].full = false; }
}

// The connection has output (or a close) for the end of this pass.
fn touch(ci: i64) {
    let c .= conns[ci];
    if (c.flags & F_TOUCHED) != 0 { return; }
    c.flags |= F_TOUCHED;
    c.mark = net_out_len(ring_of(ci), slot_of(ci)) as! u32;
    touched.push(ci);
}

fn close_after(ci: i64) {
    let c .= conns[ci];
    c.flags |= F_CLOSE;
    if (c.flags & F_KEPT) != 0 {
        net_drop_kept(ring_of(ci), slot_of(ci));
        c.flags &= ~F_KEPT;
    }
    touch(ci);
}

// After the pass's COMMIT, every touched connection's output goes to the kernel. If the commit
// failed, no answer of the pass may stand: each becomes a 500 and its connection closes.
fn flush(committed: bool) {
    for k in touched.len {
        let ci = touched[k];
        let c .= conns[ci];
        let r = ring_of(ci);
        let s = slot_of(ci);
        if !committed && (c.flags & F_ANSWERED) != 0 {
            net_out_cut(r, s, c.mark);
            net_out(r, s, err500, "");
            c.flags |= F_CLOSE;
        }
        c.flags &= ~(F_TOUCHED | F_ANSWERED);
        if (c.flags & F_OPEN) == 0 || (c.flags & (F_CLOSING | F_SENDING)) != 0 { continue; }
        if net_out_len(r, s) > 0 {
            c.flags |= F_SENDING;
            net_send(r, s, ud(OP_SEND, ci, c.gen));
        } else if (c.flags & F_CLOSE) != 0 {
            c.flags |= F_CLOSING;
            net_close(r, s, ud(OP_SHUT, ci, c.gen), ud(OP_CLOSE, ci, c.gen));
        }
    }
    touched.clear();
}

fn rearm_receives() {
    st_rearms += rearm.len;
    for k in rearm.len {
        let ci = rearm[k];
        let c = conns[ci];
        if (c.flags & F_OPEN) != 0 && (c.flags & F_CLOSING) == 0 {
            net_recv(ring_of(ci), slot_of(ci), ud(OP_RECV, ci, c.gen));
        }
    }
    rearm.clear();
}

// Keeps single-shot accepts in flight on the accepting ring, spread over the listeners. When
// that ring is nearly full, accepting moves to a ring with room, adding one if none has.
fn accept_more() {
    if rs[acc_ring].full {
        var next = -1;
        for r in nrings { if next < 0 && !rs[r].full { next = r; } }
        if next < 0 {
            if now_us < ring_retry { return; }
            if !add_ring() {
                ring_retry = now_us + 1000000;
                log("cannot add a ring; trying again in 1 s");
                return;
            }
            next = nrings - 1;
        }
        acc_ring = next;
    }
    let r = acc_ring;
    let each = depth / LISTENERS;
    for li in LISTENERS {
        while rs[r].accepts[li] < each {
            net_accept(r as! i32, li as! i32, (OP_ACCEPT << 56) | ((r as! u64) << 48) | (li as! u64));
            rs[r].accepts[li] += 1;
        }
    }
}

fn checkpoints() {
    let st = db_ck_poll();   // 0 idle, 1 copying, 2 done with the whole log copied, 3 done, frames left
    if ck_phase == 0 {
        if db_wal_frames() >= ck_start { db_ck_ask(); ck_phase = 1; }
    } else if ck_phase == 1 {
        if st >= 2 {
            // Copied up to where the log was when it started. Now nothing may be written until
            // the rest is copied, so that the log can start over.
            if !gate { gate = true; gate_since = now_us; st_gates++; }
            db_ck_ask();
            ck_phase = 2;
        } else if !gate && db_wal_frames() >= ck_high {
            gate = true;
            gate_since = now_us;
            st_gates++;
        } else if gate && now_us - gate_since > GATE_US {
            gate = false;
        }
    } else if st == 2 {
        db_wal_restarts();
        gate = false;
        ck_phase = 0;
        st_restarts++;
    } else if st == 3 {
        gate = false;
        ck_phase = 0;
    } else if gate && now_us - gate_since > GATE_US {
        gate = false;   // the copy is slow: writes go on, and a later round tries again
    }
}

fn stats() {
    st_passes++;
    let took = clock_ns() / 1000 - now_us;
    if took > st_maxpass { st_maxpass = took; }
    if st_at == 0 { st_at = now_us; }
    if now_us - st_at < 1000000 { return; }
    var open = 0;
    for r in nrings { open += rs[r].open; }
    var m: u8[>..] = [];
    format(m, "stats: ", st_reqs, " req/s, ", st_passes, " passes, ", st_cqes, " cqes, ", st_writes, " writes in ",
        st_wpasses, " write passes, max pass ", st_maxpass, " us (serve ", st_serve_us, ", writes ", st_write_us,
        ", commit ", st_commit_us, "), nobuf ", st_nobuf, ", rearms ", st_rearms,
        ", open ", open, ", rings ", nrings, ", wal ", db_wal_frames(), ", gates ", st_gates, ", restarts ",
        st_restarts, ", queued ", njobs);
    log(m);
    st_at = now_us;
    st_reqs = 0;
    st_passes = 0;
    st_cqes = 0;
    st_writes = 0;
    st_wpasses = 0;
    st_nobuf = 0;
    st_rearms = 0;
    st_maxpass = 0;
    st_serve_us = 0;
    st_write_us = 0;
    st_commit_us = 0;
}

// ---------------------------------------------------------------------------------------------
// Serving a connection's bytes

// Parses and answers every complete request in b. A write that has to wait for the next write
// pass stops it: what follows that request stays with the connection until its write is
// answered, so responses keep their order.
fn serve(ci: i64, b: const u8[:]) {
    var off = 0;
    var rq = Req { .. };
    while off < b.len {
        let rest = b[off..];
        let st = parse_request(rest, rq);
        if st == 0 { break; }
        if st < 0 {
            answer_error(ci, 400, "bad request");
            close_after(ci);
            return;
        }
        st_reqs++;
        let queued = handle(ci, rest, rq);
        off += rq.used;
        if queued {
            if off < b.len { keep(ci, b[off..], 0); }
            return;
        }
        if rq.close { close_after(ci); return; }
    }
    if off < b.len { keep(ci, b[off..], rq.need); }
}

fn keep(ci: i64, rest: const u8[:], need: i64) {
    let c .= conns[ci];
    net_keep(ring_of(ci), slot_of(ci), rest);
    c.flags |= F_KEPT;
    c.need = need as! u32;
}

// Serves the input net.c kept for the connection.
fn serve_kept(ci: i64) {
    let c .= conns[ci];
    c.flags &= ~F_KEPT;
    data.clear();
    net_kept(ring_of(ci), slot_of(ci), data);
    serve(ci, data);
}

// Runs the queued writes, in the order they came, in this pass's transaction.
fn run_writes() {
    writes_now = true;
    st_wpasses++;
    for j in jobs {
        let c .= conns[j.ci];
        if c.gen != j.gen || (c.flags & F_OPEN) == 0 || (c.flags & (F_CLOSING | F_QUEUED)) != F_QUEUED { continue; }
        c.flags &= ~F_QUEUED;
        st_writes++;
        if j.like { do_like(j.ci, j.uid, j.post_id); }
        else { do_post(j.ci, j.uid, j.who, j.text); }
        if j.close { close_after(j.ci); continue; }
        // What arrived behind the write is served now; a write among it runs at once.
        if (c.flags & F_KEPT) != 0 { serve_kept(j.ci); }
        if (c.flags & F_EOF) != 0 { close_after(j.ci); }
    }
    jobs.clear();
    njobs = 0;
    writes_now = false;
}

fn enqueue(ci: i64) {
    conns[ci].flags |= F_QUEUED;
    if njobs == 0 { write_at = now_us + write_window; }
    njobs++;
}

// ---------------------------------------------------------------------------------------------
// HTTP/1.1 framing. Only what the API uses is read: the request line, Authorization,
// Content-Length, Connection and Transfer-Encoding. Offsets are into the request's bytes.

struct Req {
    get: bool, post: bool, close: bool, has_auth: bool,
    p0: i64, p1: i64,      // path, without the query
    a0: i64, a1: i64,      // Authorization value, trimmed
    b0: i64, b1: i64,      // body
    used: i64,             // the request's length
    need: i64,             // while it is incomplete: its length, once the head says (else 0)
}

let NL = byte_set("\n");

fn trim_blanks(b: const u8[:], s: i64, e: i64) -> i64, i64 {
    var i = s;
    var j = e;
    while i < j && (b[i] == ' ' || b[i] == '\t') { i++; }
    while j > i && (b[j - 1] == ' ' || b[j - 1] == '\t') { j--; }
    return i, j;
}

// ASCII case-insensitive comparison with a lower-case name.
fn is_name(b: const u8[:], lower: const u8[:]) -> bool {
    if b.len != lower.len { return false; }
    for k in lower.len {
        var c = b[k];
        if c >= 'A' && c <= 'Z' { c += 32; }
        if c != lower[k] { return false; }
    }
    return true;
}

// 1 a whole request (r filled), 0 more bytes are needed, -1 malformed.
fn parse_request(b: const u8[:], r: Req&) -> i32 {
    r.need = 0;
    let nl = find_any(b, NL);
    if nl < 0 { return if b.len > MAX_HEAD { -1 } else { 0 }; }
    var le = nl;
    if le > 0 && b[le - 1] == '\r' { le--; }
    let line = b[..le];
    let sp1 = position(line, ' ');
    if sp1 <= 0 { return -1; }
    let sp2 = position(line[sp1 + 1..], ' ');
    if sp2 <= 0 { return -1; }
    let version = line[sp1 + sp2 + 2..];
    if !starts_with(version, "HTTP/1.") { return -1; }
    let method = line[..sp1];
    r.get = method == "GET";
    r.post = method == "POST";
    r.close = version == "HTTP/1.0";
    r.has_auth = false;
    r.p0 = sp1 + 1;
    r.p1 = sp1 + 1 + sp2;
    let q = position(b[r.p0..r.p1], '?');
    if q >= 0 { r.p1 = r.p0 + q; }
    var clen = 0;
    var pos = nl + 1;
    loop {
        let n = find_any(b[pos..], NL);
        if n < 0 { return if b.len > MAX_HEAD { -1 } else { 0 }; }
        var e = pos + n;
        let next = e + 1;
        if e > pos && b[e - 1] == '\r' { e--; }
        if e == pos { pos = next; break; }
        let colon = position(b[pos..e], ':');
        if colon > 0 {
            let name = b[pos..pos + colon];
            if colon == 13 && is_name(name, "authorization") {
                let s, t = trim_blanks(b, pos + colon + 1, e);
                r.has_auth = true;
                r.a0 = s;
                r.a1 = t;
            } else if colon == 14 && is_name(name, "content-length") {
                let s, t = trim_blanks(b, pos + colon + 1, e);
                if s == t || t - s > 9 { return -1; }
                var v = 0;
                for i in s..t {
                    if b[i] < '0' || b[i] > '9' { return -1; }
                    v = v * 10 + (b[i] - '0');
                }
                if v > MAX_BODY { return -1; }
                clen = v;
            } else if colon == 17 && is_name(name, "transfer-encoding") {
                return -1;   // chunked bodies are out of scope
            } else if colon == 10 && is_name(name, "connection") {
                let s, t = trim_blanks(b, pos + colon + 1, e);
                if is_name(b[s..t], "close") { r.close = true; }
                else if is_name(b[s..t], "keep-alive") { r.close = false; }
            }
        }
        pos = next;
        if pos > MAX_HEAD { return -1; }
    }
    if b.len < pos + clen { r.need = pos + clen; return 0; }
    r.b0 = pos;
    r.b1 = pos + clen;
    r.used = pos + clen;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Routing. Returns true when the request is a write that now waits in the write queue.

fn handle(ci: i64, b: const u8[:], r: Req&) -> bool {
    let path = b[r.p0..r.p1];
    if r.get {
        if path == "/feed" { feed(ci); return false; }
        if starts_with(path, "/posts/") {
            let rest = path[7..];
            if position(rest, '/') < 0 {
                let id = positive_int(rest);
                if id <= 0 { answer_error(ci, 400, "invalid post id"); }
                else { get_post(ci, id); }
                return false;
            }
        }
        if path == "/health" { health(ci); return false; }
    } else if r.post {
        if path == "/posts" { return create_post(ci, b, r); }
        if path.len >= 12 && starts_with(path, "/posts/") && ends_with(path, "/like") {
            let rest = path[7..path.len - 5];
            if position(rest, '/') < 0 { return like_post(ci, b, r, rest); }
        }
    }
    answer_error(ci, 404, "not found");
    return false;
}

// A positive decimal integer with no sign, spaces or leading zeros that fits an i64; else 0.
fn positive_int(s: const u8[:]) -> i64 {
    if s.len == 0 || s.len > 18 || s[0] == '0' { return 0; }
    var v = 0;
    for c in s {
        if c < '0' || c > '9' { return 0; }
        v = v * 10 + (c - '0');
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// Responses

// Appends one whole response to the connection's output; it leaves after the pass's COMMIT.
fn answer(ci: i64, status: i64, payload: const u8[:]) {
    touch(ci);
    conns[ci].flags |= F_ANSWERED;
    head.clear();
    head.append(match status {
        200 => "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ",
        201 => "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ",
        400 => "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ",
        401 => "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ",
        404 => "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ",
        503 => "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ",
        _ => "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ",
    });
    format(head, payload.len);
    head.append("\r\n\r\n");
    net_out(ring_of(ci), slot_of(ci), head, payload);
}

fn answer_error(ci: i64, status: i64, message: const u8[:]) {
    body.clear();
    body.append("{\"error\":\"");
    body.append(message);
    body.append("\"}");
    answer(ci, status, body);
}

fn answer_500(ci: i64) { answer_error(ci, 500, "internal server error"); }

let HEX = "0123456789abcdef";
let ONES: u64 = 0x0101010101010101;
let HIGHS: u64 = 0x8080808080808080;

// The first eight bytes of s, the first lowest.
inline fn load64(s: const u8[:]) -> u64 {
    assert(s.len >= 8);
    return s[0] | ((s[1] as u64) << 8) | ((s[2] as u64) << 16) | ((s[3] as u64) << 24) |
        ((s[4] as u64) << 32) | ((s[5] as u64) << 40) | ((s[6] as u64) << 48) | ((s[7] as u64) << 56);
}

// Nonzero when one of the eight bytes of x is a control character, a quote or a backslash:
// the bytes JSON escapes inside a string.
inline fn special8(x: u64) -> u64 {
    let quote = x ^ (ONES * 0x22);
    let slash = x ^ (ONES * 0x5c);
    return (((x - ONES * 0x20) & ~x) | ((quote - ONES) & ~quote) | ((slash - ONES) & ~slash)) & HIGHS;
}

// The same test for one byte, without a branch: bit 7 is set when c needs escaping.
inline fn special1(c: u8) -> u8 {
    let quote = c ^ 0x22;
    let slash = c ^ 0x5c;
    return ((c - 0x20) & ~c) | ((quote - 1) & ~quote) | ((slash - 1) & ~slash);
}

// True when no byte of s needs escaping. 32 bytes and more go through loops of a fixed 32 that
// the C compiler turns into vector instructions; shorter strings go eight bytes at a time. The
// last block or word overlaps the one before it instead of being cut short.
fn is_plain(s: const u8[:]) -> bool {
    let n = s.len;
    if n >= 32 {
        // One accumulator per byte position, so that it stays in a vector register from block
        // to block and is folded only once.
        var seen: u8[32] = [0; 32];
        var rest = s;
        while rest.len >= 32 {
            let part = rest[..32];
            for k in 32 { seen[k] |= special1(part[k]); }
            rest = rest[32..];
        }
        let last = s[n - 32..];
        assert(last.len == 32);
        var wide: u8 = 0;
        for k in 32 { wide |= seen[k] | special1(last[k]); }
        return (wide & 0x80) == 0;
    }
    if n >= 8 {
        var rest = s;
        var found: u64 = 0;
        while rest.len >= 8 {
            found |= special8(load64(rest));
            rest = rest[8..];
        }
        return (found | special8(load64(s[n - 8..]))) == 0;
    }
    for c in s { if c < 0x20 || c == '"' || c == '\\' { return false; } }
    return true;
}

// Appends s as the inside of a JSON string.
fn json_text(out: u8[>..]&, s: const u8[:]) {
    if is_plain(s) { out.append(s); return; }
    for c in s {
        if c == '"' { out.append("\\\""); }
        else if c == '\\' { out.append("\\\\"); }
        else if c >= 0x20 { out.push(c); }
        else if c == '\n' { out.append("\\n"); }
        else if c == '\r' { out.append("\\r"); }
        else if c == '\t' { out.append("\\t"); }
        else if c == 8 { out.append("\\b"); }
        else if c == 12 { out.append("\\f"); }
        else {
            out.append("\\u00");
            out.push(HEX[c >> 4]);
            out.push(HEX[c & 15]);
        }
    }
}

// One post object. `pair` is the post's body directly followed by its created_at, as they lie
// in a posts record, so one scan clears both; the body is its first body_len bytes.
fn json_post(out: u8[>..]&, id: i64, pair: const u8[:], body_len: i64, author: const u8[:], likes: i64) {
    out.append("{\"id\":");
    format(out, id);
    out.append(",\"body\":\"");
    if is_plain(pair) {
        out.append(pair[..body_len]);
        out.append("\",\"created_at\":\"");
        out.append(pair[body_len..]);
    } else {
        json_text(out, pair[..body_len]);
        out.append("\",\"created_at\":\"");
        json_text(out, pair[body_len..]);
    }
    out.append("\",\"author\":\"");
    json_text(out, author);
    out.append("\",\"like_count\":");
    format(out, likes);
    out.push('}');
}

// ---------------------------------------------------------------------------------------------
// SQLite's record format: a header of varints (its own length, then one serial type per
// column), then the column values in order.

// The varint at b[i]: its value and the index after it (-1 if it runs off the end).
inline fn vint(b: const u8[:], i: i64) -> i64, i64 {
    if i < b.len && b[i] < 0x80 { return b[i], i + 1; }
    return vint_long(b, i);
}

fn vint_long(b: const u8[:], i: i64) -> i64, i64 {
    var v: u64 = 0;
    for k in 9 {
        if i + k >= b.len { return 0, -1; }
        let c = b[i + k];
        if k == 8 { return ((v << 8) | c) as! i64, i + 9; }
        v = (v << 7) | (c & 0x7f);
        if c < 0x80 { return v as! i64, i + k + 1; }
    }
    return 0, -1;
}

let SERIAL_SIZES: i64[12] = [0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0];

inline fn serial_size(t: i64) -> i64 {
    return if t >= 12 { (t - 12) / 2 } else if t >= 0 { SERIAL_SIZES[t] } else { 0 };
}

// The first n (at most 4) columns of the record at b[at..at + len]: serial types, and where
// each value starts and ends in b.
struct Cols { t: i64[4], s: i64[4], e: i64[4] }

fn columns(b: const u8[:], at: i64, len: i64, n: i64, c: Cols&) -> bool {
    assert(n >= 0 && n <= 4);
    let r = b[at..at + len];
    // Short headers (every record of this schema): the header's length is one byte, and a
    // serial type is one or two.
    if len < 1 || r[0] >= 0x80 { return columns_any(b, at, len, n, c); }
    let hsize: i64 = r[0];
    if hsize > len { return false; }
    var h = 1;
    var v = hsize;
    for k in n {
        if h >= hsize { return false; }
        var t: i64 = r[h];
        h++;
        if t >= 0x80 {
            if h >= hsize { return false; }
            let low: i64 = r[h];
            h++;
            if low >= 0x80 { return columns_any(b, at, len, n, c); }
            t = ((t & 0x7f) << 7) | low;
        }
        let size = serial_size(t);
        if v + size > len { return false; }
        c.t[k] = t;
        c.s[k] = at + v;
        c.e[k] = at + v + size;
        v += size;
    }
    return true;
}

// The same for any record.
fn columns_any(b: const u8[:], at: i64, len: i64, n: i64, c: Cols&) -> bool {
    assert(n >= 0 && n <= 4);
    let r = b[at..at + len];
    let hsize, h0 = vint(r, 0);
    if h0 < 0 || hsize > len { return false; }
    var h = h0;
    var v = hsize;
    for k in n {
        if h >= hsize { return false; }
        let t, next = vint(r[..hsize], h);
        if next < 0 { return false; }
        h = next;
        let size = serial_size(t);
        if v + size > len { return false; }
        c.t[k] = t;
        c.s[k] = at + v;
        c.e[k] = at + v + size;
        v += size;
    }
    return true;
}

inline fn is_text(t: i64) -> bool { return t >= 13 && (t & 1) == 1; }

// A column as an integer (serial types 1-6, 8 and 9).
fn int_column(b: const u8[:], t: i64, s: i64) -> i64, bool {
    if t == 8 { return 0, true; }
    if t == 9 { return 1, true; }
    if t < 1 || t > 6 { return 0, false; }
    let n = serial_size(t);
    var v: i64 = (b[s] as! i8);
    for k in 1..n { v = (v << 8) | b[s + k]; }
    return v, true;
}

// ---------------------------------------------------------------------------------------------
// Reads. Each moves SQLite's cursors inside the pass's transaction and builds its response from
// the rows it reads there; nothing is remembered between requests.

// The leading integer column of the index entry the cursor is on (post_id in
// likes_post_id_idx, whose entries are two integers: at most 19 bytes).
fn index_key(tree: i32) -> i64, bool {
    var e: u8[24] = [0; 24];
    let len = db_row_start(tree, e);
    let at: i64 = e[0];
    let t: i64 = e[1];
    if len >= 3 && len <= 24 && at >= 2 && at <= 16 && t <= 9 {
        if t == 8 { return 0, true; }
        if t == 9 { return 1, true; }
        if t < 1 || t > 6 || at + SERIAL_SIZES[t] > len { return 0, false; }
        // The value is big-endian at e[at]: take eight bytes and keep its own.
        let w = e[at..at + 8];
        let x = ((w[0] as u64) << 56) | ((w[1] as u64) << 48) | ((w[2] as u64) << 40) | ((w[3] as u64) << 32) |
            ((w[4] as u64) << 24) | ((w[5] as u64) << 16) | ((w[6] as u64) << 8) | w[7];
        return (x as! i64) >> ((64 - 8 * SERIAL_SIZES[t]) as! u64), true;
    }
    if len < 0 { return 0, false; }
    // An entry of another shape: decode the whole record.
    var c = Cols { .. };
    rec.clear();
    let whole = db_row(tree, rec);
    if whole < 0 || !columns(rec, 0, whole, 1, c) { return 0, false; }
    let v, ok = int_column(rec, c.t[0], c.s[0]);
    return v, ok;
}

// The likes of one post: its entries in likes_post_id_idx (post_id, rowid). -1 on an error.
fn count_likes(id: i64) -> i64 {
    var at = db_seek_ge(LIKES, id);
    var n = 0;
    while at == 1 {
        let v, ok = index_key(LIKES);
        if !ok { return -1; }
        if v != id { break; }
        n++;
        at = db_next(LIKES);
    }
    return if at < 0 { -1 } else { n };
}

// Appends the username of user `uid` to `text`: where it starts and ends, and 1; 0 if there is
// no such user, -1 on an error.
fn author_of(uid: i64) -> i64, i64, i32 {
    let at = db_seek(USERS, uid);
    if at != 1 { return 0, 0, at; }
    let start = text.len;
    let len = db_row(USERS, text);
    var c = Cols { .. };
    if len < 0 || !columns(text, start, len, 2, c) || !is_text(c.t[1]) { return 0, 0, -1; }
    return c.s[1], c.e[1], 1;
}

fn get_post(ci: i64, id: i64) {
    if db_begin() != 0 { answer_500(ci); return; }
    let at = db_seek(POSTS, id);
    if at == 0 { answer_error(ci, 404, "post not found"); return; }
    if at < 0 { answer_500(ci); return; }
    text.clear();
    let len = db_row(POSTS, text);
    var c = Cols { .. };
    // posts: id (the rowid, stored as NULL), user_id, body, created_at
    if len < 0 || !columns(text, 0, len, 4, c) || !is_text(c.t[2]) || !is_text(c.t[3]) { answer_500(ci); return; }
    let uid, uid_ok = int_column(text, c.t[1], c.s[1]);
    if !uid_ok { answer_500(ci); return; }
    let a0, a1, found = author_of(uid);
    if found == 0 { answer_error(ci, 404, "post not found"); return; }   // the JOIN finds no row
    if found < 0 { answer_500(ci); return; }
    let likes = count_likes(id);
    if likes < 0 { answer_500(ci); return; }
    body.clear();
    body.append("{\"post\":");
    json_post(body, id, text[c.s[2]..c.e[3]], c.e[2] - c.s[2], text[a0..a1], likes);
    body.push('}');
    answer(ci, 200, body);
}

// The 20 newest posts by (created_at DESC, id DESC), which is the order of
// posts_created_at_id_idx, so the plan is: the index's first 20 entries, their posts by rowid in
// ascending order (neighbours on a leaf are one step apart), their authors, their like counts.
// Nothing about the data is assumed. 1 rows filled, 0 the reference query must answer, -1 error.
fn feed_rows() -> i32 {
    text.clear();
    nrows = 0;
    var c = Cols { .. };
    var at = db_first(FEED);
    while at == 1 && nrows < FEED_LEN {
        rec.clear();
        let len = db_row(FEED, rec);
        // the index: created_at, id, rowid
        if len < 0 || !columns(rec, 0, len, 2, c) { return -1; }
        let id, ok = int_column(rec, c.t[1], c.s[1]);
        if !ok { return -1; }
        rows[nrows] = Row { id: id, .. };
        nrows++;
        at = db_next(FEED);
    }
    if at < 0 { return -1; }
    let n = nrows;
    assert(n >= 0 && n <= FEED_LEN);
    // Newest first is normally descending ids, so ascending is the reverse.
    for k in n { order[k] = n - 1 - k; }
    for k in 1..n {
        let o = order[k];
        var j = k;
        while j > 0 && rows[order[j - 1]].id > rows[o].id { order[j] = order[j - 1]; j--; }
        order[j] = o;
    }
    for k in n {
        let row .= rows[order[k]];
        let found = db_seek(POSTS, row.id);
        if found < 0 { return -1; }
        if found == 0 { return 0; }   // the index and the table disagree
        let start = text.len;
        let len = db_row(POSTS, text);
        if len < 0 || !columns(text, start, len, 4, c) || !is_text(c.t[2]) || !is_text(c.t[3]) { return -1; }
        let uid, ok = int_column(text, c.t[1], c.s[1]);
        if !ok { return -1; }
        row.uid = uid;
        row.b0 = c.s[2];
        row.b1 = c.e[2];
        row.c1 = c.e[3];
    }
    for k in n {
        let row .= rows[k];
        let a0, a1, found = author_of(row.uid);
        if found < 0 { return -1; }
        if found == 0 { return 0; }   // a post without a user: the JOIN drops it
        row.a0 = a0;
        row.a1 = a1;
    }
    if n == 0 { return 1; }
    let lo = rows[order[0]].id;
    let hi = rows[order[n - 1]].id;
    if hi - lo > LIKE_SPAN {
        for k in n {
            let likes = count_likes(rows[k].id);
            if likes < 0 { return -1; }
            rows[k].likes = likes;
        }
        return 1;
    }
    // One walk of the index from the lowest id to the highest counts them all.
    for k in hi - lo + 1 { like_counts[k] = 0; }
    at = db_seek_ge(LIKES, lo);
    while at == 1 {
        let v, ok = index_key(LIKES);
        if !ok { return -1; }
        if v > hi { break; }
        like_counts[v - lo] += 1;
        at = db_next(LIKES);
    }
    if at < 0 { return -1; }
    for k in n { rows[k].likes = like_counts[rows[k].id - lo]; }
    return 1;
}

fn feed(ci: i64) {
    if db_begin() != 0 { answer_500(ci); return; }
    let got = feed_rows();
    if got < 0 { answer_500(ci); return; }
    body.clear();
    body.append("{\"posts\":[");
    if got == 1 {
        for k in nrows {
            let row = rows[k];
            if k > 0 { body.push(','); }
            json_post(body, row.id, text[row.b0..row.c1], row.b1 - row.b0, text[row.a0..row.a1], row.likes);
        }
    } else if !feed_by_query() {
        answer_500(ci);
        return;
    }
    body.append("]}");
    answer(ci, 200, body);
}

// The reference SQL query, for a feed the cursor plan does not answer.
fn feed_by_query() -> bool {
    var row = FeedRow { .. };
    var first = true;
    loop {
        text.clear();
        let st = db_feed_step(row, text);
        if st == 0 { return true; }
        if st < 0 { return false; }
        if !first { body.push(','); }
        first = false;
        let b1 = row.body_len;
        let c1 = b1 + row.created_len;
        json_post(body, row.id, text[..c1], b1, text[c1..c1 + row.author_len], row.likes);
    }
}

fn health(ci: i64) {
    var err: u8[>..] = [];
    body.clear();
    if db_begin() != 0 { err.append("cannot begin a transaction"); }
    else if db_ping(err) == 0 {
        format(body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", (clock_ns() / 1000 - started_us) / 1000000, "}");
        answer(ci, 200, body);
        return;
    }
    body.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    json_text(body, err);
    body.append("\"}");
    answer(ci, 503, body);
}

// ---------------------------------------------------------------------------------------------
// Writes

// Checks the token and answers its failures. The user id (the username is in `who`), or 0.
fn authorize(ci: i64, b: const u8[:], r: Req&) -> i64 {
    let uid, why = verify(if r.has_auth { b[r.a0..r.a1] } else { b[..0] }, r.has_auth);
    if why == 0 { return uid; }
    answer_error(ci, 401, match why {
        1 => "missing bearer token",
        2 => "invalid or expired token",
        _ => "invalid token payload",
    });
    return 0;
}

fn create_post(ci: i64, b: const u8[:], r: Req&) -> bool {
    let uid = authorize(ci, b, r);
    if uid == 0 { return false; }
    let payload = b[r.b0..r.b1];
    var m = Member { .. };
    var none = Member { .. };
    let shape = json_members(payload, 1, "body", m, "", none, "", none);
    if shape < 0 { answer_error(ci, 400, "malformed JSON body"); return false; }
    if shape == 0 || m.kind != J_STRING { answer_error(ci, 400, "body is required"); return false; }
    unesc.clear();
    if m.esc { json_unescape(payload[m.s..m.e], unesc); }
    let raw = if m.esc { unesc[..] } else { payload[m.s..m.e] };
    let t0, t1 = js_trim(raw);
    if t0 == t1 { answer_error(ci, 400, "body is required"); return false; }
    let trimmed = raw[t0..t1];
    if trimmed.len > MAX_CHARS && code_points(trimmed) > MAX_CHARS {
        answer_error(ci, 400, "body must be at most 500 characters");
        return false;
    }
    if writes_now { do_post(ci, uid, who, trimmed); return false; }
    jobs.push(Job { ci: ci, gen: conns[ci].gen, like: false, close: r.close, uid: uid, post_id: 0, who: who[..], text: trimmed });
    enqueue(ci);
    return true;
}

fn like_post(ci: i64, b: const u8[:], r: Req&, id_text: const u8[:]) -> bool {
    let uid = authorize(ci, b, r);
    if uid == 0 { return false; }
    let id = positive_int(id_text);
    if id <= 0 { answer_error(ci, 400, "invalid post id"); return false; }
    if writes_now { do_like(ci, uid, id); return false; }
    jobs.push(Job { ci: ci, gen: conns[ci].gen, like: true, close: r.close, uid: uid, post_id: id, who: "", text: "" });
    enqueue(ci);
    return true;
}

fn do_post(ci: i64, uid: i64, author: const u8[:], text_: const u8[:]) {
    if db_begin() != 0 { answer_500(ci); return; }
    var id = 0;
    created.clear();
    if db_add_post(uid, text_, id, created) != 0 { answer_500(ci); return; }
    body.clear();
    body.append("{\"post\":");
    // the body and created_at side by side, as json_post takes them
    text.clear();
    text.append(text_);
    text.append(created);
    json_post(body, id, text, text_.len, author, 0);
    body.push('}');
    answer(ci, 201, body);
}

fn do_like(ci: i64, uid: i64, id: i64) {
    if db_begin() != 0 { answer_500(ci); return; }
    let found = db_seek(POSTS, id);
    if found == 0 { answer_error(ci, 404, "post not found"); return; }
    if found < 0 { answer_500(ci); return; }
    let added = db_add_like(uid, id);
    if added < 0 { answer_500(ci); return; }
    body.clear();
    body.append(if added == 1 { "{\"liked\":true,\"already_liked\":false,\"post_id\":" }
                else { "{\"liked\":true,\"already_liked\":true,\"post_id\":" });
    format(body, id);
    body.push('}');
    answer(ci, if added == 1 { 201 } else { 200 }, body);
}

// JavaScript's String.prototype.trim: WhiteSpace and LineTerminator code points.
fn js_space(cp: i64) -> bool {
    if cp <= 0x20 { return cp == 0x20 || (cp >= 9 && cp <= 13); }
    if cp < 0xa0 { return false; }
    return cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

// The code point of the (valid) UTF-8 sequence at s[i] and its length.
fn utf8_at(s: const u8[:], i: i64) -> i64, i64 {
    let c: i64 = s[i];
    if c < 0x80 { return c, 1; }
    if c < 0xe0 { return ((c & 0x1f) << 6) | (s[i + 1] & 0x3f), 2; }
    if c < 0xf0 { return ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) as i64 << 6) | (s[i + 2] & 0x3f), 3; }
    return ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) as i64 << 12) | ((s[i + 2] & 0x3f) as i64 << 6) | (s[i + 3] & 0x3f), 4;
}

// Where s starts and ends once trimmed (s is valid UTF-8).
fn js_trim(s: const u8[:]) -> i64, i64 {
    var i = 0;
    var j = s.len;
    while i < j {
        let cp, n = utf8_at(s, i);
        if !js_space(cp) { break; }
        i += n;
    }
    while j > i {
        var k = j - 1;
        while k > i && (s[k] & 0xc0) == 0x80 { k--; }
        let cp, n = utf8_at(s, k);
        if !js_space(cp) { break; }
        j = k;
    }
    return i, j;
}

fn code_points(s: const u8[:]) -> i64 {
    var n = 0;
    for c in s { if (c & 0xc0) != 0x80 { n++; } }
    return n;
}

// ---------------------------------------------------------------------------------------------
// JSON: a validating reader. A document is checked whole (any nesting, with an explicit stack,
// so no input can recurse), and the members of a top-level object that the caller names are
// reported. Member names are compared decoded, and the last duplicate wins, as in JSON.parse.

let J_NONE = 0;
let J_STRING = 1;
let J_NUMBER = 2;
let J_OTHER = 3;

// A member's value: for a string, s..e is its inside (still escaped when esc); for a number,
// its text.
struct Member { kind: i64, s: i64, e: i64, esc: bool }

inline fn json_ws(b: const u8[:], at: i64) -> i64 {
    var i = at;
    while i < b.len && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t') { i++; }
    return i;
}

fn hex_digit(c: u8) -> i64 {
    if c >= '0' && c <= '9' { return c - '0'; }
    if c >= 'a' && c <= 'f' { return c - 'a' + 10; }
    if c >= 'A' && c <= 'F' { return c - 'A' + 10; }
    return -1;
}

// The length of the well-formed UTF-8 sequence at b[i] (a lead byte >= 0x80), or -1.
fn utf8_len(b: const u8[:], i: i64) -> i64 {
    let c = b[i];
    if c < 0xc2 { return -1; }
    if c < 0xe0 {
        if i + 1 >= b.len || (b[i + 1] & 0xc0) != 0x80 { return -1; }
        return 2;
    }
    if c < 0xf0 {
        if i + 2 >= b.len || (b[i + 1] & 0xc0) != 0x80 || (b[i + 2] & 0xc0) != 0x80 { return -1; }
        if c == 0xe0 && b[i + 1] < 0xa0 { return -1; }    // overlong
        if c == 0xed && b[i + 1] >= 0xa0 { return -1; }   // a surrogate
        return 3;
    }
    if c > 0xf4 || i + 3 >= b.len { return -1; }
    if (b[i + 1] & 0xc0) != 0x80 || (b[i + 2] & 0xc0) != 0x80 || (b[i + 3] & 0xc0) != 0x80 { return -1; }
    if c == 0xf0 && b[i + 1] < 0x90 { return -1; }        // overlong
    if c == 0xf4 && b[i + 1] >= 0x90 { return -1; }       // past U+10FFFF
    return 4;
}

// Bytes that end a run of plain string content: the quote, the backslash, control characters and
// everything outside ASCII.
fn string_stops() -> bool[256] {
    var m: bool[256] = [false; 256];
    for i in 32 { m[i] = true; }
    for i in 128..256 { m[i] = true; }
    m['"'] = true;
    m['\\'] = true;
    return m;
}
let STOPS = byte_set(string_stops());

// The string whose opening quote is at b[at]: the index after its closing quote (-1 if it is
// not a valid string) and whether it has escapes.
fn json_string(b: const u8[:], at: i64) -> i64, bool {
    var i = at + 1;
    var esc = false;
    loop {
        let k = find_any(b[i..], STOPS);
        if k < 0 { return -1, false; }
        i += k;
        let c = b[i];
        if c == '"' { return i + 1, esc; }
        if c < 0x20 { return -1, false; }
        if c == '\\' {
            esc = true;
            if i + 1 >= b.len { return -1, false; }
            let d = b[i + 1];
            if d == 'u' {
                if i + 5 >= b.len { return -1, false; }
                for h in 2..6 { if hex_digit(b[i + h]) < 0 { return -1, false; } }
                i += 6;
            } else if d == '"' || d == '\\' || d == '/' || d == 'b' || d == 'f' || d == 'n' || d == 'r' || d == 't' {
                i += 2;
            } else {
                return -1, false;
            }
        } else {
            let n = utf8_len(b, i);
            if n < 0 { return -1, false; }
            i += n;
        }
    }
}

// The number at b[at]: the index after it, or -1.
fn json_number(b: const u8[:], at: i64) -> i64 {
    var i = at;
    if i < b.len && b[i] == '-' { i++; }
    if i >= b.len { return -1; }
    if b[i] == '0' { i++; }
    else if b[i] >= '1' && b[i] <= '9' { while i < b.len && b[i] >= '0' && b[i] <= '9' { i++; } }
    else { return -1; }
    if i < b.len && b[i] == '.' {
        i++;
        if i >= b.len || b[i] < '0' || b[i] > '9' { return -1; }
        while i < b.len && b[i] >= '0' && b[i] <= '9' { i++; }
    }
    if i < b.len && (b[i] == 'e' || b[i] == 'E') {
        i++;
        if i < b.len && (b[i] == '+' || b[i] == '-') { i++; }
        if i >= b.len || b[i] < '0' || b[i] > '9' { return -1; }
        while i < b.len && b[i] >= '0' && b[i] <= '9' { i++; }
    }
    return i;
}

// A member name at b[at], then a colon: the index after the colon, or -1.
fn json_name(b: const u8[:], at: i64) -> i64 {
    if at >= b.len || b[at] != '"' { return -1; }
    let e, esc = json_string(b, at);
    if e < 0 { return -1; }
    let i = json_ws(b, e);
    if i >= b.len || b[i] != ':' { return -1; }
    return i + 1;
}

let MAX_DEPTH = 512;

// The value at b[at] (after blanks): the index after it, or -1 if it is not valid JSON.
fn json_value(b: const u8[:], at: i64) -> i64 {
    var open: u8[512] = [0; 512];   // 1 an object, 2 an array
    var depth = 0;
    var want = true;
    var i = at;
    loop {
        i = json_ws(b, i);
        if i >= b.len { return -1; }
        let c = b[i];
        if want {
            if c == '{' || c == '[' {
                if depth == MAX_DEPTH { return -1; }
                open[depth] = if c == '{' { 1 } else { 2 };
                depth++;
                i = json_ws(b, i + 1);
                if i >= b.len { return -1; }
                if b[i] == (if c == '{' { '}' } else { ']' }) {
                    i++;
                    depth--;
                    want = false;
                } else if c == '{' {
                    i = json_name(b, i);
                    if i < 0 { return -1; }
                }
            } else {
                if c == '"' {
                    let e, esc = json_string(b, i);
                    i = e;
                } else if c == '-' || (c >= '0' && c <= '9') {
                    i = json_number(b, i);
                } else if starts_with(b[i..], "true") || starts_with(b[i..], "null") {
                    i += 4;
                } else if starts_with(b[i..], "false") {
                    i += 5;
                } else {
                    return -1;
                }
                if i < 0 { return -1; }
                want = false;
            }
        } else {
            let top = open[depth - 1];
            if c == ',' {
                i++;
                if top == 1 {
                    i = json_name(b, json_ws(b, i));
                    if i < 0 { return -1; }
                }
                want = true;
            } else if (top == 1 && c == '}') || (top == 2 && c == ']') {
                i++;
                depth--;
            } else {
                return -1;
            }
        }
        if !want && depth == 0 { return i; }
    }
}

fn name_matches(b: const u8[:], s: i64, e: i64, esc: bool, key: const u8[:]) -> bool {
    if !esc { return b[s..e] == key; }
    keybuf.clear();
    json_unescape(b[s..e], keybuf);
    return keybuf == key;
}

// Checks the document b and reports up to three members of its top-level object by name
// (nkeys of them are looked for). 1 an object, 0 valid JSON of another kind, -1 not JSON.
fn json_members(b: const u8[:], nkeys: i64, k0: const u8[:], m0: Member&, k1: const u8[:], m1: Member&,
                k2: const u8[:], m2: Member&) -> i32 {
    m0.kind = J_NONE;
    m1.kind = J_NONE;
    m2.kind = J_NONE;
    var i = json_ws(b, 0);
    if i >= b.len { return -1; }
    if b[i] != '{' {
        i = json_value(b, i);
        if i < 0 || json_ws(b, i) != b.len { return -1; }
        return 0;
    }
    i = json_ws(b, i + 1);
    if i < b.len && b[i] == '}' {
        return if json_ws(b, i + 1) == b.len { 1 } else { -1 };
    }
    loop {
        if i >= b.len || b[i] != '"' { return -1; }
        let ne, nesc = json_string(b, i);
        if ne < 0 { return -1; }
        let n0 = i + 1;
        let n1 = ne - 1;
        i = json_ws(b, ne);
        if i >= b.len || b[i] != ':' { return -1; }
        let v0 = json_ws(b, i + 1);
        if v0 >= b.len { return -1; }
        var m = Member { kind: J_OTHER, s: v0, e: 0, esc: false };
        if b[v0] == '"' {
            let se, sesc = json_string(b, v0);
            if se < 0 { return -1; }
            m = Member { kind: J_STRING, s: v0 + 1, e: se - 1, esc: sesc };
            i = se;
        } else {
            i = json_value(b, v0);
            if i < 0 { return -1; }
            m.e = i;
            if b[v0] == '-' || (b[v0] >= '0' && b[v0] <= '9') { m.kind = J_NUMBER; }
        }
        if nkeys > 0 && name_matches(b, n0, n1, nesc, k0) { m0 = m; }
        else if nkeys > 1 && name_matches(b, n0, n1, nesc, k1) { m1 = m; }
        else if nkeys > 2 && name_matches(b, n0, n1, nesc, k2) { m2 = m; }
        i = json_ws(b, i);
        if i >= b.len { return -1; }
        if b[i] == '}' {
            return if json_ws(b, i + 1) == b.len { 1 } else { -1 };
        }
        if b[i] != ',' { return -1; }
        i = json_ws(b, i + 1);
    }
}

// Decodes the inside of a string that json_string accepted. A surrogate escape without its pair
// becomes U+FFFD, as it does when JavaScript writes the string out as UTF-8.
fn json_unescape(s: const u8[:], out: u8[>..]&) {
    var i = 0;
    while i < s.len {
        let c = s[i];
        if c != '\\' { out.push(c); i++; continue; }
        let d = s[i + 1];
        i += 2;
        if d == 'u' {
            var cp = (hex_digit(s[i]) << 12) | (hex_digit(s[i + 1]) << 8) | (hex_digit(s[i + 2]) << 4) | hex_digit(s[i + 3]);
            i += 4;
            if cp >= 0xd800 && cp < 0xdc00 && i + 5 < s.len && s[i] == '\\' && s[i + 1] == 'u' {
                let lo = (hex_digit(s[i + 2]) << 12) | (hex_digit(s[i + 3]) << 8) | (hex_digit(s[i + 4]) << 4) | hex_digit(s[i + 5]);
                if lo >= 0xdc00 && lo < 0xe000 {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                }
            }
            if cp >= 0xd800 && cp < 0xe000 { cp = 0xfffd; }
            push_utf8(out, cp);
        } else {
            out.push(match d { 'b' => 8, 'f' => 12, 'n' => 10, 'r' => 13, 't' => 9, _ => d });
        }
    }
}

// ---------------------------------------------------------------------------------------------
// HS256. Every token's signature is checked on every request.

let K256: u32[64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

let SHA_INIT: u32[8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

inline fn rotr(x: u32, n: u32) -> u32 { return (x >> n) | (x << (32 - n)); }

// One SHA-256 compression of the 64 bytes at b[at].
fn sha_block(h: u32[8]&, b: const u8[:], at: i64) {
    assert(at >= 0 && at + 64 <= b.len);
    var w: u32[64] = [0; 64];
    for i in 16 {
        let j = at + i * 4;
        w[i] = ((b[j] as u32) << 24) | ((b[j + 1] as u32) << 16) | ((b[j + 2] as u32) << 8) | b[j + 3];
    }
    for i in 16..64 {
        let s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        let s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    var a = h[0];
    var bb = h[1];
    var c = h[2];
    var d = h[3];
    var e = h[4];
    var f = h[5];
    var g = h[6];
    var hh = h[7];
    for i in 64 {
        let t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        let t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = bb;
        bb = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

// Finishes a hash whose state h has taken `before` bytes already, with msg as the rest.
fn sha_finish(h: u32[8]&, msg: const u8[:], before: i64, out: u8[32]&) {
    var at = 0;
    while at + 64 <= msg.len { sha_block(h, msg, at); at += 64; }
    var last: u8[128] = [0; 128];
    let rest = msg.len - at;
    assert(rest >= 0 && rest < 64);
    last[..rest].move_from(msg[at..]);
    last[rest] = 0x80;
    let total = if rest < 56 { 64 } else { 128 };
    let bits = ((before + msg.len) * 8) as! u64;
    for i in 8 { last[total - 1 - i] = ((bits >> ((i * 8) as! u64)) & 0xff) as! u8; }
    sha_block(h, last, 0);
    if total == 128 { sha_block(h, last, 64); }
    for i in 8 {
        out[i * 4] = (h[i] >> 24) as! u8;
        out[i * 4 + 1] = ((h[i] >> 16) & 0xff) as! u8;
        out[i * 4 + 2] = ((h[i] >> 8) & 0xff) as! u8;
        out[i * 4 + 3] = (h[i] & 0xff) as! u8;
    }
}

// Hashes the key's two padded blocks once: every HMAC then starts from these states.
fn hmac_key(secret: const u8[:]) {
    var key: u8[64] = [0; 64];
    if secret.len > 64 {
        var h = SHA_INIT;
        var d: u8[32] = [0; 32];
        sha_finish(h, secret, 0, d);
        for i in 32 { key[i] = d[i]; }
    } else {
        for i in secret.len { key[i] = secret[i]; }
    }
    var kpad: u8[64] = [0; 64];
    for i in 64 { kpad[i] = key[i] ^ 0x36; }
    hmac_inner = SHA_INIT;
    sha_block(hmac_inner, kpad, 0);
    for i in 64 { kpad[i] = key[i] ^ 0x5c; }
    hmac_outer = SHA_INIT;
    sha_block(hmac_outer, kpad, 0);
}

fn hmac(msg: const u8[:], out: u8[32]&) {
    var h = hmac_inner;
    var inner: u8[32] = [0; 32];
    sha_finish(h, msg, 64, inner);
    h = hmac_outer;
    sha_finish(h, inner, 64, out);
}

// Decodes unpadded base64url, appending to out. Only the canonical spelling is accepted (the
// unused bits of the last character zero), so one spelling of a signature verifies.
fn base64url(s: const u8[:], out: u8[>..]&) -> bool {
    let tail = s.len % 4;
    if tail == 1 { return false; }
    let whole = s.len - tail;
    var i = 0;
    while i < whole {
        let a = b64[s[i]];
        let b = b64[s[i + 1]];
        let c = b64[s[i + 2]];
        let d = b64[s[i + 3]];
        if ((a | b | c | d) & 0xc0) != 0 { return false; }
        let v = ((a as u32) << 18) | ((b as u32) << 12) | ((c as u32) << 6) | d;
        out.push((v >> 16) as! u8);
        out.push(((v >> 8) & 0xff) as! u8);
        out.push((v & 0xff) as! u8);
        i += 4;
    }
    if tail == 0 { return true; }
    let a = b64[s[i]];
    let b = b64[s[i + 1]];
    let c = if tail == 3 { b64[s[i + 2]] } else { 0 };
    if ((a | b | c) & 0xc0) != 0 { return false; }
    let v = ((a as u32) << 18) | ((b as u32) << 12) | ((c as u32) << 6);
    if (if tail == 2 { v & 0xffff } else { v & 0xff }) != 0 { return false; }
    out.push((v >> 16) as! u8);
    if tail == 3 { out.push(((v >> 8) & 0xff) as! u8); }
    return true;
}

// The decoded text of a string member.
fn member_text(src: const u8[:], m: Member&, out: u8[>..]&) {
    if m.esc { json_unescape(src[m.s..m.e], out); }
    else { out.append(src[m.s..m.e]); }
}

// Verifies an Authorization header value. Returns the user id and 0, leaving the username in
// `who`; or 0 and 1 no bearer token, 2 invalid or expired, 3 a bad payload.
fn verify(header: const u8[:], present: bool) -> i64, i32 {
    if !present || !starts_with(header, "Bearer ") { return 0, 1; }
    let token = header[7..];
    let dot1 = position(token, '.');
    if dot1 < 0 { return 0, 2; }
    let dot2 = position(token[dot1 + 1..], '.');
    if dot2 < 0 { return 0, 2; }
    let sig = token[dot1 + dot2 + 2..];
    if position(sig, '.') >= 0 { return 0, 2; }
    jwt_head.clear();
    if !base64url(sig, jwt_head) || jwt_head.len != 32 { return 0, 2; }
    var want: u8[32] = [0; 32];
    hmac(token[..dot1 + 1 + dot2], want);
    var diff: u8 = 0;
    for i in 32 { diff |= want[i] ^ jwt_head[i]; }   // every byte, whatever the first difference
    if diff != 0 { return 0, 2; }

    var alg = Member { .. };
    var exp = Member { .. };
    var sub = Member { .. };
    var name = Member { .. };
    jwt_head.clear();
    if !base64url(token[..dot1], jwt_head) { return 0, 2; }
    if json_members(jwt_head, 1, "alg", alg, "", exp, "", sub) != 1 || alg.kind != J_STRING { return 0, 2; }
    who.clear();
    member_text(jwt_head, alg, who);
    if who != "HS256" { return 0, 2; }

    jwt_body.clear();
    if !base64url(token[dot1 + 1..dot1 + 1 + dot2], jwt_body) { return 0, 2; }
    if json_members(jwt_body, 3, "exp", exp, "sub", sub, "username", name) != 1 { return 0, 2; }
    if exp.kind != J_NUMBER { return 0, 2; }
    let until, exp_ok = parse_flt(jwt_body[exp.s..exp.e]);
    if !exp_ok || !(time() < until) { return 0, 2; }
    if sub.kind != J_STRING || name.kind != J_STRING { return 0, 3; }
    who.clear();
    member_text(jwt_body, sub, who);
    let uid = positive_int(who);
    if uid <= 0 { return 0, 3; }
    who.clear();
    member_text(jwt_body, name, who);
    return uid, 0;
}
