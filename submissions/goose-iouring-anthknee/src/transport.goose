// Imperative shell. Rust's receive strategy, Goose-owned state and fair bounded queues.
import std;
import os;
import src.model;
import src.native;
import src.input_buffers;
import src.app;
namespace transport;

let ACCEPT: u64 = 1;
let RECEIVE: u64 = 2;
let READ: u64 = 3;
let SEND: u64 = 4;
let SHUTDOWN: u64 = 5;
let CLOSE: u64 = 6;
let CANCEL_ACCEPT: u64 = 7;
let MORE: u32 = 2;
let BUFFER: u32 = 1;
let NOT_FOUND: i32 = -2;
let AGAIN: i32 = -11;
let ALREADY: i32 = -114;
let NO_BUFFERS: i32 = -105;

struct Ring { open: i64, reads: i64, accepting: bool[8], cancelling: bool[8] }
var connections: model::Connection[>..];
var rings: Ring[16];
var ring_count: i64 = 0;
var slots: i64 = 0;
var accept_ring: i64 = 0;
var margin: i64 = 0;
var retry_at: i64 = 0;
var answers_left: i64 = 0;
var started: i64 = 0;
var completions: native::Cqe[>..];
var ready: model::Handle[>..<];
var reads: model::Handle[>..<];
var held: model::Handle[>..<];
var touched: model::Handle[>..<];
var ready_head: i64 = 0;
var read_head: i64 = 0;
var held_head: i64 = 0;
var incoming: u8[>..];
var assembled: u8[>..];
var response: u8[>..];
var header: u8[>..];

fn fatal(message: const u8[:], result: i32) {
    var text: u8[>..] = [];
    format(text, message, " (", result, ")");
    native::log(text);
    exit(1);
}

fn queued(result: i32) { if result < 0 { fatal("io_uring submission failed", result); } }

fn empty(generation: u32) -> model::Connection {
    return model::Connection { generation: generation, life: model::Life.Free,
        rx: model::Rx.End, tx: model::Tx.Idle, input: model::Input.Empty,
        out: -1, rollback_at: 0, flags: 0 };
}

fn handle(index: i64) -> model::Handle {
    assert(index >= 0); assert(index < connections.len);
    return model::Handle { index: index as! u32, generation: connections[index].generation };
}

fn current(value: model::Handle) -> bool {
    return value.index < connections.len && connections[value.index].generation == value.generation;
}

fn live(value: model::Handle) -> bool {
    return current(value) && connections[value.index].life == model::Life.Open;
}

// 8 operation bits, 8 ring bits, 32 generation bits and 16 slot bits.
fn tag(operation: u64, index: i64) -> u64 {
    let h = handle(index);
    return (operation << 56) | (((index / slots) as u64) << 48) |
        ((h.generation as u64) << 16) | ((index % slots) as u64);
}

fn input_box(input: model::Input) -> i32 {
    return match input { Empty => -1, Partial v => v.box, Ready v => v.box, Held v => v.box };
}

fn compact(queue: model::Handle[>..<]&, head: i64&) {
    assert(head >= 0); assert(head <= queue.len);
    if head == queue.len { queue.clear(); head = 0; }
    else if head >= 4096 && head * 2 >= queue.len {
        let remaining = queue.len - head;
        for i in remaining { queue[i] = queue[head + i]; }
        queue.resize(remaining);
        head = 0;
    }
}

fn touch(index: i64) {
    let c .= connections[index];
    if (c.flags & model::TOUCHED) != 0 { return; }
    c.flags |= model::TOUCHED;
    c.rollback_at = if c.out >= 0 { native::box_len(c.out) } else { 0 };
    touched.push(handle(index));
}

fn close_after(index: i64) {
    connections[index].flags |= model::CLOSE_AFTER;
    touch(index);
}

fn status_line(status: i32) -> const u8[:] {
    return match status {
        200 => "HTTP/1.1 200 OK\r\n", 201 => "HTTP/1.1 201 Created\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n", 401 => "HTTP/1.1 401 Unauthorized\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n", 503 => "HTTP/1.1 503 Service Unavailable\r\n",
        _ => "HTTP/1.1 500 Internal Server Error\r\n",
    };
}

fn append_response(index: i64, status: i32, body: const u8[:]) -> bool {
    touch(index);
    let c .= connections[index];
    header.clear();
    header.append(status_line(status));
    header.append("Content-Type: application/json\r\nContent-Length: ");
    json::push_int(header, body.len);
    header.append("\r\n\r\n");
    if c.out < 0 { c.out = native::box_new(header.len + body.len); }
    if c.out < 0 || !native::box_append2(c.out, header, body) { close_after(index); return false; }
    c.flags |= model::ANSWERED;
    return true;
}

fn stash(index: i64, bytes: const u8[:], kind: i32) {
    let c .= connections[index];
    assert(c.life == model::Life.Open);
    var box = input_box(c.input);
    if box < 0 {
        box = input_buffers::save(bytes);
        if box < 0 { close_after(index); return; }
    } else if !input_buffers::append(box, bytes) {
        input_buffers::release(box); c.input = model::Input.Empty;
        close_after(index); return;
    }
    // Each transition into a queued state contributes exactly one generation-tagged handle.
    if kind == 0 { c.input = model::Input.Partial { box: box }; }
    else if kind == 1 {
        c.input = model::Input.Ready { box: box };
        if ready.len - ready_head >= connections.len {
            input_buffers::release(box); c.input = model::Input.Empty; close_after(index); return;
        }
        ready.push(handle(index));
    } else {
        c.input = model::Input.Held { box: box };
        if held.len - held_head >= connections.len {
            input_buffers::release(box); c.input = model::Input.Empty; close_after(index); return;
        }
        held.push(handle(index));
    }
}

// A pass processes at most ANSWERS attempts. A request and later bytes stay together.
fn serve(index: i64, bytes: const u8[:], floor: i64) {
    assert(floor >= 0);
    if (connections[index].flags & model::CLOSE_AFTER) != 0 { return; }
    var offset = 0;
    while offset < bytes.len {
        let remaining = bytes[offset..];
        if answers_left <= floor { stash(index, remaining, 1); return; }
        let result = http::parse(remaining);
        match result {
            NeedMore => { stash(index, remaining, 0); return; },
            Invalid => {
                answers_left--;
                response.clear();
                let status = app::error(response, 400, "bad request");
                // If output allocation fails, append_response marks the connection to close.
                append_response(index, status, response);
                close_after(index); return;
            },
            Complete parsed => {
                var req = parsed.request;
                assert(req.used > 0); assert(req.used <= remaining.len);
                answers_left--;
                if req.method == model::Method.Post && db::writes_held() {
                    stash(index, remaining, 2); return;
                }
                let status = app::respond(remaining, req, started, response);
                if !append_response(index, status, response) { return; }
                offset += req.used;
                if req.close { close_after(index); return; }
            },
        }
    }
}

fn append_waiting(index: i64, box: i32, bytes: const u8[:]) {
    var next = box;
    if !input_buffers::append(next, bytes) { close_after(index); return; }
    if next != box {
        // Growing input can move from the bounded native path into the Goose pool.
        match connections[index].input {
            Ready => { connections[index].input = model::Input.Ready { box: next }; },
            Held => { connections[index].input = model::Input.Held { box: next }; },
            _ => { assert(false); },
        }
    }
}

fn received(index: i64, bytes: const u8[:]) {
    let c .= connections[index];
    if (c.flags & model::CLOSE_AFTER) != 0 { return; }
    match c.input {
        Ready v => { append_waiting(index, v.box, bytes); },
        Held v => { append_waiting(index, v.box, bytes); },
        Partial v => {
            assembled.clear(); input_buffers::read(v.box, assembled); assembled.append(bytes);
            input_buffers::release(v.box); c.input = model::Input.Empty;
            serve(index, assembled, 0);
        },
        Empty => { serve(index, bytes, 0); },
    }
}

fn queue_read(index: i64) {
    let c .= connections[index];
    if c.life != model::Life.Open || c.rx == model::Rx.Queued || c.rx == model::Rx.Reading ||
        (c.flags & (model::CLOSE_AFTER | model::PEER_EOF)) != 0 { return; }
    if reads.len - read_head >= connections.len { close_after(index); return; }
    c.rx = model::Rx.Queued;
    reads.push(handle(index));
}

fn eof(index: i64) {
    let c .= connections[index];
    c.rx = model::Rx.End;
    c.flags |= model::PEER_EOF;
    match c.input {
        Empty, Partial => { close_after(index); },
        Ready, Held => {}, // Already read requests still receive their responses.
    }
}

fn arm_receive(index: i64) {
    let c .= connections[index];
    if (c.flags & (model::CLOSE_AFTER | model::PEER_EOF)) != 0 { c.rx = model::Rx.End; return; }
    c.rx = model::Rx.Multi;
    queued(native::recv((index / slots) as! i32, (index % slots) as! i32, tag(RECEIVE, index)));
}

fn on_receive(ring: i64, index: i64, generation: u32, result: i32, flags: u32,
              single: bool) {
    if single { assert(rings[ring].reads > 0); rings[ring].reads--; }
    let selected = (flags & BUFFER) != 0;
    let buffer = (flags >> 16) as! i32;
    let h = model::Handle { index: index as! u32, generation: generation };
    if !live(h) {
        if selected { native::return_buffer(ring as! i32, buffer); }
        return;
    }
    let c .= connections[index];
    if single || (flags & MORE) == 0 { c.rx = model::Rx.End; }
    if result > 0 && selected {
        incoming.clear(); native::take(ring as! i32, buffer, result, incoming);
        received(index, incoming);
        if single {
            if result == native::receive_buffer_size() { queue_read(index); } else { arm_receive(index); }
        } else if (flags & MORE) == 0 { queue_read(index); }
    } else {
        if selected { native::return_buffer(ring as! i32, buffer); }
        if result == NO_BUFFERS || result > 0 { queue_read(index); }
        else if single && result == AGAIN { arm_receive(index); }
        else { eof(index); }
    }
}

fn on_send(index: i64, generation: u32, result: i32) {
    let h = model::Handle { index: index as! u32, generation: generation };
    if !current(h) { return; }
    let c .= connections[index];
    match c.tx {
        Idle => {}, // A stale completion cannot release a buffer a later send owns.
        Flight flight => {
            if result <= 0 {
                native::box_drop(flight.box); c.tx = model::Tx.Idle;
                if c.out >= 0 { native::box_drop(c.out); c.out = -1; }
                close_after(index);
            } else {
                let sent = flight.sent + result as! u32;
                assert(sent <= native::box_len(flight.box));
                if sent < native::box_len(flight.box) {
                    c.tx = model::Tx.Flight { box: flight.box, sent: sent };
                    queued(native::send((index / slots) as! i32, (index % slots) as! i32,
                        flight.box, sent, tag(SEND, index)));
                } else {
                    native::box_drop(flight.box); c.tx = model::Tx.Idle;
                    if c.out >= 0 || (c.flags & model::CLOSE_AFTER) != 0 { touch(index); }
                }
            }
        },
    }
}

fn on_close(index: i64, generation: u32) {
    let h = model::Handle { index: index as! u32, generation: generation };
    if !current(h) || connections[index].life == model::Life.Free { return; }
    let c = connections[index];
    assert(c.life == model::Life.Closing);
    match c.tx { Idle => {}, Flight => { assert(false); } }
    if c.out >= 0 { native::box_drop(c.out); }
    let box = input_box(c.input);
    if box >= 0 { input_buffers::release(box); }
    connections[index] = empty(generation);
    assert(rings[index / slots].open > 0); rings[index / slots].open--;
}

fn listener_tag(operation: u64, ring: i64, listener: i64) -> u64 {
    return (operation << 56) | ((ring as u64) << 48) | (listener as u64);
}

fn cancel_accept(ring: i64, listener: i64) {
    if !rings[ring].accepting[listener] || rings[ring].cancelling[listener] { return; }
    queued(native::cancel_accept(ring as! i32, listener_tag(ACCEPT, ring, listener),
        listener_tag(CANCEL_ACCEPT, ring, listener)));
    rings[ring].cancelling[listener] = true;
}

fn drain(ring: i64) {
    completions.clear(); native::reap(ring as! i32, completions);
    for event in completions {
        let operation = event.ud >> 56;
        let slot = (event.ud & 0xffff) as! i64;
        let generation = ((event.ud >> 16) & 0xffffffff) as! u32;
        let index = ring * slots + slot;
        if operation == ACCEPT {
            assert(slot < 8); assert(rings[ring].accepting[slot]);
            if (event.flags & MORE) == 0 { rings[ring].accepting[slot] = false; }
            // A final success still owns a slot, even if this ring is retiring.
            if event.result >= 0 {
                let ci = ring * slots + event.result;
                assert(event.result < slots); assert(connections[ci].life == model::Life.Free);
                let gen = connections[ci].generation + 1;
                connections[ci] = empty(gen);
                connections[ci].life = model::Life.Open;
                rings[ring].open++;
                arm_receive(ci);
            }
        } else if operation == CANCEL_ACCEPT {
            assert(slot < 8); assert(rings[ring].cancelling[slot]);
            rings[ring].cancelling[slot] = false;
            // An executing accept can race cancellation. Its own final CQE clears accepting.
            if event.result == ALREADY {
                if ring != accept_ring { cancel_accept(ring, slot); }
            } else if event.result < 0 && event.result != NOT_FOUND {
                fatal("cancel accept failed", event.result);
            }
        } else if operation == RECEIVE || operation == READ {
            on_receive(ring, index, generation, event.result, event.flags, operation == READ);
        } else if operation == SEND { on_send(index, generation, event.result); }
        else if operation == CLOSE { on_close(index, generation); }
        else { assert(operation == SHUTDOWN); } // Its linked close completion owns cleanup.
    }
}

fn run_ready() {
    let end = ready.len;
    while ready_head < end && answers_left > 0 {
        let h = ready[ready_head]; ready_head++;
        if !live(h) { continue; }
        let c .= connections[h.index];
        match c.input {
            Empty, Partial, Held => {},
            Ready v => {
                assembled.clear(); input_buffers::read(v.box, assembled);
                input_buffers::release(v.box); c.input = model::Input.Empty;
                serve(h.index, assembled, 0);
                if (c.flags & model::PEER_EOF) != 0 {
                    match c.input { Empty, Partial => { close_after(h.index); }, Ready, Held => {} }
                }
            },
        }
    }
    compact(ready, ready_head);
}

fn release_held() {
    if db::writes_held() { return; }
    let end = held.len;
    // Reserve part of the old-work budget for reads when both queues have work.
    let floor = if ready_head < ready.len { answers_left / 2 } else { 0 };
    while held_head < end && answers_left > floor {
        let h = held[held_head]; held_head++;
        if !live(h) { continue; }
        let c .= connections[h.index];
        match c.input {
            Empty, Partial, Ready => {},
            Held v => {
                assembled.clear(); input_buffers::read(v.box, assembled);
                input_buffers::release(v.box); c.input = model::Input.Empty;
                serve(h.index, assembled, floor);
                if (c.flags & model::PEER_EOF) != 0 {
                    match c.input { Empty, Partial => { close_after(h.index); }, Ready, Held => {} }
                }
            },
        }
    }
    compact(held, held_head);
}

fn read_rounds(first_ring: i64, reads_left: i64&, pause_for_replies: bool) -> bool {
    // Each productive round issues at least one read, bounded by READS per pass.
    while read_head < reads.len && reads_left > 0 {
        if pause_for_replies && answers_left == 0 && touched.len > 0 { return true; }
        let end = reads.len;
        var round = 0;
        while read_head < end && reads_left > 0 {
            let h = reads[read_head]; read_head++;
            if !live(h) { continue; }
            let c .= connections[h.index];
            if c.rx != model::Rx.Queued { continue; }
            let ring = h.index / slots;
            if rings[ring].reads >= model::MAX_RECOVERY_READS_PER_RING { reads.push(h); continue; }
            if (c.flags & (model::CLOSE_AFTER | model::PEER_EOF)) != 0 { c.rx = model::Rx.End; continue; }
            c.rx = model::Rx.Reading; rings[ring].reads++;
            queued(native::read(ring as! i32, (h.index % slots) as! i32, tag(READ, h.index)));
            reads_left--; round++;
        }
        if round == 0 { break; }
        native::publish();
        for offset in ring_count {
            let ring = (first_ring + offset) % ring_count;
            queued(native::enter(ring as! i32, false));
            drain(ring);
        }
    }
    compact(reads, read_head);
    return false;
}

fn flush(committed: bool) {
    for h in touched {
        if !live(h) { continue; }
        let c .= connections[h.index];
        if !committed && (c.flags & model::ANSWERED) != 0 && c.out >= 0 {
            native::box_cut(c.out, c.rollback_at);
            let failed = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";
            if !native::box_append(c.out, failed) { native::box_drop(c.out); c.out = -1; }
            c.flags |= model::CLOSE_AFTER;
        }
        c.flags &= ~(model::TOUCHED | model::ANSWERED);
        match c.tx {
            Flight => {},
            Idle => {
                if c.out >= 0 && native::box_len(c.out) > 0 {
                    let box = c.out; c.out = -1;
                    c.tx = model::Tx.Flight { box: box, sent: 0 };
                    queued(native::send((h.index / slots) as! i32, (h.index % slots) as! i32,
                        box, 0, tag(SEND, h.index)));
                } else if (c.flags & model::CLOSE_AFTER) != 0 {
                    c.life = model::Life.Closing;
                    queued(native::close((h.index / slots) as! i32, (h.index % slots) as! i32,
                        tag(SHUTDOWN, h.index), tag(CLOSE, h.index)));
                }
            },
        }
    }
    touched.clear();
}

fn add_ring() -> bool {
    if ring_count >= model::MAX_RINGS { return false; }
    let result = native::ring_add(slots as! i32);
    if result < 0 { return false; }
    assert(result == ring_count);
    connections.push_n(empty(0), slots);
    rings[ring_count] = Ring { open: 0, reads: 0, accepting: [false; 8], cancelling: [false; 8] };
    ring_count++;
    return true;
}

fn accept_more(now: i64) {
    if rings[accept_ring].open + margin >= slots {
        for listener in 8 { cancel_accept(accept_ring, listener); }
        var next = -1;
        for i in ring_count { if next < 0 && rings[i].open + margin * 2 < slots { next = i; } }
        if next < 0 {
            if now < retry_at { return; }
            if !add_ring() { retry_at = now + 1000000; return; }
            next = ring_count - 1;
        }
        accept_ring = next;
    }
    let ring = accept_ring;
    for listener in 8 {
        // Reuse the tag only after both the stream and any cancellation have completed.
        if rings[ring].accepting[listener] || rings[ring].cancelling[listener] { continue; }
        queued(native::accept(ring as! i32, listener as! i32, listener_tag(ACCEPT, ring, listener)));
        rings[ring].accepting[listener] = true;
    }
}

fn initialize() {
    var path: u8[>..] = [];
    var secret: u8[>..] = [];
    var host: u8[>..] = [];
    var port: u8[>..] = [];
    if !native::env("SQLITE_PATH", path) || path.len == 0 { fatal("SQLITE_PATH is required", 0); }
    if !native::env("JWT_SECRET", secret) || secret.len == 0 { fatal("JWT_SECRET is required", 0); }
    if !native::env("HOST", host) { host.append("0.0.0.0"); }
    if !native::env("PORT", port) { port.append("80"); }
    let number = model::positive_int(port);
    if number <= 0 || number > 65535 { fatal("PORT must be 1-65535", 0); }
    var message: u8[>..] = [];
    if native::open(path, message) != 0 { native::log(message); exit(1); }
    app::initialize(secret);
    slots = native::slots();
    if slots < 128 { fatal("descriptor limit must be at least 128", slots as! i32); }
    margin = min(1024, slots / 8);
    let listening = native::listen(host, number as! i32);
    if listening < 0 { fatal("listen failed", listening); }
    if !add_ring() { fatal("io_uring setup failed", 0); }
    native::lock_code();
    started = native::micros();
    format(message, "Goose io_uring: ", slots, " direct slots per ring");
    native::log(message);
    accept_more(started);
}

fn run() {
    initialize();
    var first_ring: i64 = 0;
    // Intentional process-lifetime event loop. All per-pass work and queues are bounded.
    loop {
        let old_work = ready.len - ready_head + (if db::writes_held() { 0 } else { held.len - held_head });
        let busy = old_work > 0 || read_head < reads.len;
        var ring = ring_count;
        while ring > 0 { ring--; queued(native::enter(ring as! i32, ring == 0 && !busy)); }
        answers_left = model::ANSWERS;
        if old_work > 0 {
            let reserved = min(model::ANSWERS / 2, old_work);
            answers_left = reserved;
            release_held();
            run_ready();
            // Carry unused reserved attempts into the rest of this pass.
            answers_left += model::ANSWERS - reserved;
        }
        for offset in ring_count {
            drain((first_ring + offset) % ring_count);
        }
        release_held();
        run_ready();
        var reads_left = model::READS;
        let recovery_paused = read_rounds(first_ring, reads_left, true);
        native::publish();
        let committed = native::commit();
        flush(committed);
        if recovery_paused {
            queued(native::submit_pending());
            read_rounds(first_ring, reads_left, false);
            native::publish();
            // With no answer budget left, completions can expose committed output or a close.
            flush(true);
        }
        first_ring = (first_ring + 1) % ring_count;
        let now = native::micros();
        db::tick(now);
        accept_more(now);
        if native::trim_due(now) {
            native::heap_trim();
            // Call through Goose so the compiler publishes live stack tops.
            trim_memory();
        }
        if native::stop() { native::shutdown(); return; } // Join the checkpoint worker before leaving the loop.
    }
}
