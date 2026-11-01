import application;

// Only fragmented, pipelined or budget-deferred input gets owned storage.
// Relative links are four bytes; no C pointer can refer into these pools.
struct Fragment { storage: u8[:], used: u32 }
reusable[] var input_bytes: u8[>..] = [];
reusable var fragments: Fragment[>..] = [];
struct Connection {
    generation: u32, last_active: u32, flags: u16, reserved: u16,
    input: Fragment&<u32 in fragments>?,
}
var connections: Connection[>..] = [];
type ConnectionId = u32 in connections;
struct Handle { index: u32, generation: u32 }

// These are responses already produced in execution order, waiting for commit.
// Error records cost 11 bytes; successful responses cost 22 bytes.
// The aggregate payload also makes Goose 2026.3 use a packed-struct copy,
// avoiding its unaligned scalar stores when constructing a variable enum.
struct Delivery { connection: Handle, arena: u32, offset: u32, len: u32, database: bool }
enum Reply {
    Error { connection: Handle, code: u8, database: bool },
    Data { value: Delivery },
}
var replies: Reply..[>..] = [];
var ready: Handle[>..<] = [];
var held: Handle[>..<] = [];
var reads: Handle[>..<] = [];
var rearm: Handle[>..<] = [];
var read_head: i64 = 0;
var clock_s: u32 = 0;
var writes_allowed = true;
var answers_left: u32 = 0;
var ready_reserved: u32 = 0;

const OPEN: u16 = 1;
const SENDING: u16 = 2;
const CLOSING: u16 = 4;
const CLOSE_AFTER: u16 = 8;
const REPLIED: u16 = 16;
const READY: u16 = 32;
const HELD: u16 = 64;
const REARM: u16 = 128;
const READ_QUEUED: u16 = 256;
const RECEIVING: u16 = 512;
const ANSWER_BUDGET: u32 = 2048;
const READY_SHARE: u32 = 1024;
const PEND_MAX = 1064960;

extern fn g300_native_poll(index: u32, generation: u32);
extern fn g300_native_close(index: u32, generation: u32);
extern fn g300_native_read(index: u32, generation: u32) -> bool;
extern fn g300_native_send(index: u32, generation: u32, arena: u32,
                           offset: u32, len: u32, error: u8);
extern fn g300_native_release_batch();
extern fn g300_native_valve() -> bool;

fn handle(id: ConnectionId) -> Handle { return Handle { index: id, generation: connections[id].generation }; }
fn current(h: Handle) -> bool {
    return h.index < connections.len && connections[h.index].generation == h.generation;
}
fn live(h: Handle) -> bool {
    return current(h) && (connections[h.index].flags & (OPEN | CLOSING)) == OPEN;
}
fn close_connection(id: ConnectionId) {
    if (connections[id].flags & (OPEN | CLOSING)) != OPEN { return; }
    connections[id].flags |= CLOSING;
    g300_native_close(id, connections[id].generation);
}
fn queue_read(id: ConnectionId) {
    if (connections[id].flags & READ_QUEUED) != 0 { return; }
    connections[id].flags |= READ_QUEUED;
    reads.push(handle(id));
}
fn queue_ready(id: ConnectionId) {
    if (connections[id].flags & READY) != 0 { return; }
    connections[id].flags |= READY;
    ready.push(handle(id));
}
fn release_input(id: ConnectionId) {
    let f = connections[id].input;
    if f {
        input_bytes.free_slice(f.storage);
        f.used = 0;
        fragments.free(fragments.index_of(f));
        connections[id].input .= null;
    }
}
fn append_input(id: ConnectionId, data: const u8[:]) {
    if data.len == 0 { return; }
    let old = connections[id].input;
    if !old {
        var capacity: i64 = 512;
        while capacity < data.len { capacity *= 2; }
        connections[id].input .= fragments.alloc_ref(Fragment {
            storage: input_bytes.alloc_slice(capacity), used: 0 });
    }
    let f = connections[id].input;
    assert(f);
    let used = f.used as i64;
    let needed = used + data.len;
    if needed > 0xFFFFFFFF { close_connection(id); return; }
    if needed > f.storage.len {
        var capacity = f.storage.len;
        while capacity < needed { capacity *= 2; }
        f.storage = input_bytes.realloc_slice(f.storage, capacity);
    }
    for c, i in data { f.storage[used + i] = c; }
    f.used = needed as u32;
}
fn defer_answer(id: ConnectionId, answer: Output) {
    let h = handle(id);
    if answer.error != 0 {
        replies.push(Reply.Error { connection: h, code: answer.error, database: answer.database });
    } else {
        replies.push(Reply.Data { value: Delivery { connection: h, arena: answer.arena,
            offset: answer.offset, len: answer.len, database: answer.database } });
    }
    connections[id].flags |= REPLIED;
}

// Consumes at most one request, as in the reference. A borrowed receive span
// never escapes. Only its unconsumed suffix enters the fragment pool.
fn consume(id: ConnectionId, data: const u8[:]) -> i64 {
    if data.len == 0 || (connections[id].flags & (SENDING | REPLIED | CLOSING | HELD)) != 0 { return 0; }
    var r = g300_native_parse(data);
    if r.status == 0 {
        if data.len > PEND_MAX { close_connection(id); return data.len; }
        return 0;
    }
    if r.status < 0 {
        defer_answer(id, Output { error: BAD_HTTP, .. });
        connections[id].flags |= CLOSE_AFTER;
        return data.len;
    }
    if r.keep_alive && r.total == data.len && g300_native_valve() { r.keep_alive = false; }
    let answer = application(data, r, writes_allowed);
    if answer.held {
        connections[id].flags |= HELD;
        held.push(handle(id));
        return 0;
    }
    defer_answer(id, answer.output);
    if !r.keep_alive { connections[id].flags |= CLOSE_AFTER; }
    return r.total;
}
fn feed(id: ConnectionId, data: const u8[:]) {
    let existing = connections[id].input;
    if existing {
        append_input(id, data);
        let f = connections[id].input;
        assert(f);
        let used = f.used as i64;
        let consumed = consume(id, f.storage[..used]);
        if consumed == used { release_input(id); }
        else if consumed > 0 {
            for i in used - consumed { f.storage[i] = f.storage[consumed + i]; }
            f.used = (used - consumed) as u32;
        }
    } else {
        let consumed = consume(id, data);
        append_input(id, data[consumed..]);
    }
}

export fn g300_ring_created(ring: u32) {
    assert(ring < 16);
    let end = ((ring as i64) + 1) * 65536;
    while connections.len < end { connections.push(Connection { .. }); }
}
export fn g300_prewarm() {
    let zero = Handle { index: 0, generation: 0 };
    ready.resize(32768, zero); ready.clear();
    held.resize(32768, zero); held.clear();
    reads.resize(32768, zero); reads.clear();
    rearm.resize(4096, zero); rearm.clear();
    for i in 32768 {
        replies.push(Reply.Data { value: Delivery { connection: zero, arena: 0, offset: 0, len: 0, database: false } });
    }
    replies.clear();
}
export fn g300_turn(now: u32, writes_ok: bool) {
    clock_s = now;
    writes_allowed = writes_ok;
    answers_left = ANSWER_BUDGET;
    // Keep a share for work already queued before this turn. New receives can
    // use the rest; unused capacity is available to the ready list below.
    ready_reserved = READY_SHARE;
    if ready.len < (READY_SHARE as i64) { ready_reserved = ready.len as u32; }
}
export fn g300_accept(index: u32) -> bool {
    let id = index as ConnectionId;
    if (connections[id].flags & OPEN) != 0 { return false; }
    connections[id].flags = OPEN;
    connections[id].last_active = clock_s;
    assert(!connections[id].input);
    g300_native_poll(id, connections[id].generation);
    return true;
}
export fn g300_poll(index: u32, generation: u32, result: i32, more: bool) {
    let h = Handle { index: index, generation: generation };
    if !live(h) { return; }
    let id = index as ConnectionId;
    if result < 0 { close_connection(id); return; }
    queue_read(id);
    if !more && (connections[id].flags & REARM) == 0 {
        connections[id].flags |= REARM;
        rearm.push(h);
    }
}
export fn g300_receive(index: u32, generation: u32, result: i32,
                       data: const u8[:], full: bool) {
    if !live(Handle { index: index, generation: generation }) { return; }
    let id = index as ConnectionId;
    connections[id].flags &= ~RECEIVING;
    if result > 0 {
        connections[id].last_active = clock_s;
        if answers_left > ready_reserved {
            answers_left -= 1;
            feed(id, data);
        } else {
            append_input(id, data);
            queue_ready(id);
        }
        if full { queue_read(id); }
    } else if result == -11 { // EAGAIN
    } else if result == -105 { // ENOBUFS, keep the reference's retry policy
        queue_read(id);
    } else { close_connection(id); }
}
// Native code calls this only after the final send CQE, including partial sends.
export fn g300_sent(index: u32, generation: u32, failed: bool) {
    let h = Handle { index: index, generation: generation };
    if !current(h) { return; }
    let id = index as ConnectionId;
    connections[id].flags &= ~SENDING;
    if failed || (connections[id].flags & CLOSE_AFTER) != 0 { close_connection(id); return; }
    if connections[id].input && (connections[id].flags & (READY | HELD | CLOSING)) == 0 { queue_ready(id); }
}
export fn g300_closed(index: u32, generation: u32, result: i32) -> bool {
    if !current(Handle { index: index, generation: generation }) { return false; }
    let id = index as ConnectionId;
    if (connections[id].flags & CLOSING) == 0 { return false; }
    if result == -107 || result == -32 || result == -88 { return false; }
    release_input(id);
    connections[id].generation = ((generation as u64 + 1) & 0xFFFFFFFF) as u32;
    connections[id].flags = 0;
    // Native send tickets retain their arenas until their own final CQE, even
    // if the descriptor slot is already reused with a new generation.
    return true;
}
export fn g300_issue_reads() -> u32 {
    var issued: u32 = 0;
    let end = reads.len;
    while read_head < end && issued < 16384 {
        let h = reads[read_head];
        read_head += 1;
        if !live(h) { continue; }
        let id = h.index as ConnectionId;
        if (connections[id].flags & RECEIVING) != 0 || !g300_native_read(id, h.generation) {
            reads.push(h);
        } else {
            connections[id].flags = (connections[id].flags & ~READ_QUEUED) | RECEIVING;
            issued += 1;
        }
    }
    if read_head == reads.len { reads.clear(); read_head = 0; }
    else if read_head > reads.len / 2 {
        let count = reads.len - read_head;
        for i in count { reads[i] = reads[read_head + i]; }
        reads.resize(count, Handle { .. });
        read_head = 0;
    }
    return issued;
}
export fn g300_run_lists() {
    let end = ready.len;
    var i: i64 = 0;
    while i < end && answers_left > 0 {
        let h = ready[i];
        i += 1;
        if !current(h) { continue; }
        let id = h.index as ConnectionId;
        connections[id].flags &= ~READY;
        if (connections[id].flags & (OPEN | CLOSING | SENDING | HELD)) != OPEN { continue; }
        answers_left -= 1;
        feed(id, "");
    }
    let count = ready.len - i;
    for j in count { ready[j] = ready[i + j]; }
    ready.resize(count, Handle { .. });
    if writes_allowed {
        let n = held.len;
        for j in n {
            let h = held[j];
            if !current(h) { continue; }
            let id = h.index as ConnectionId;
            connections[id].flags &= ~HELD;
            if live(h) { feed(id, ""); }
        }
        held.clear();
    }
}
fn send_reply(h: Handle, database: bool, failed: bool,
              arena: u32, offset: u32, len: u32, error: u8) {
    if !current(h) { return; }
    let id = h.index as ConnectionId;
    connections[id].flags &= ~REPLIED;
    if !live(h) { return; }
    if database && failed { g300_native_send(id, h.generation, 0, 0, 0, INTERNAL); }
    else { g300_native_send(id, h.generation, arena, offset, len, error); }
    connections[id].flags |= SENDING;
}
export fn g300_flush(failed: bool) {
    for r in replies {
        match r {
            Error &e => { send_reply(e.connection, e.database, failed, 0, 0, 0, e.code); },
            Data &d => { send_reply(d.value.connection, d.value.database, failed,
                                    d.value.arena, d.value.offset, d.value.len, 0); },
        }
    }
    replies.clear();
    g300_native_release_batch();
}
export fn g300_rearm() {
    for h in rearm {
        if !live(h) { continue; }
        let id = h.index as ConnectionId;
        if (connections[id].flags & REARM) != 0 {
            g300_native_poll(id, h.generation);
            connections[id].flags &= ~REARM;
        }
    }
    rearm.clear();
}
export fn g300_sweep() {
    for c, i in connections {
        if (c.flags & (OPEN | CLOSING)) == OPEN &&
           ((clock_s as u64 + 0x100000000 - c.last_active) & 0xFFFFFFFF) > 90 {
            close_connection(i as ConnectionId);
        }
    }
}
export fn g300_busy() -> bool { return held.len != 0 || ready.len != 0 || reads.len > read_head; }
export fn g300_send_live(index: u32, generation: u32) -> bool {
    return live(Handle { index: index, generation: generation });
}
