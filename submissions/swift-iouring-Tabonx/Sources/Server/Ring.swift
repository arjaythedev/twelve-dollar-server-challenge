// io_uring HTTP/1.1 server around the feed service.
//
// One thread drives every ring. Each ring has its own registered file table, so accepted sockets live
// in the ring (direct descriptors) instead of the process descriptor table, and a ring holds up to
// RLIMIT_NOFILE connections. Rings are added as they fill.
//
// A pass collects the completions of every ring, answers every request in them inside one SQLite
// transaction, commits, and only then queues the sends. Responses are written into an arena that
// stays alive until its last send completes, so nothing is copied between building a response and
// sending it.

#if os(Linux)
import CShim
import CUring

enum Op: UInt64 {
    case accept = 1, recv, send, close
}

/// user_data layout: op (8 bits) | generation (16) | slot (24) | arena (16).
@inline(__always)
func tag(_ op: Op, slot: UInt32 = 0, generation: UInt16 = 0, arena: UInt16 = 0) -> UInt64 {
    op.rawValue | UInt64(generation) << 8 | UInt64(slot) << 24 | UInt64(arena) << 48
}

struct Slot {
    var open = false
    var generation: UInt16 = 0
    var receiving = false
    /// A send is in flight from `sendArena` (or from `overflow` when `sendArena` is `ownBuffer`).
    var sending = false
    /// A write waits, unread, for the checkpoint hold to end; later bytes queue behind it.
    var parked = false
    var closeAfterSend = false
    var sendArena: UInt16 = 0
    var sendRemaining: Int32 = 0
    var sendPointer: UnsafeMutablePointer<UInt8>? = nil
    /// Bytes of a request that hasn't fully arrived (or that waits while parked).
    var input: UnsafeMutablePointer<UInt8>? = nil
    var inputCount: Int32 = 0
    var inputCapacity: Int32 = 0
    /// Responses produced while a send was in flight; sent when it completes.
    var overflowCount: Int32 = 0
    var overflowCapacity: Int32 = 0
    var overflow: UnsafeMutablePointer<UInt8>? = nil
    /// A slot-owned buffer currently being sent (former overflow).
    var sendInFlightOwned: UnsafeMutablePointer<UInt8>? = nil
}

/// Output memory for one pass. Reused once every send that points into it has completed.
final class Arena {
    var out = OutBuffer(capacity: 1 << 20)
    var pendingSends = 0
}

struct Response {
    var ring: Int32
    var slot: UInt32
    var start: Int
    var end: Int
    var close: Bool
}

struct Parked {
    var ring: Int32
    var slot: UInt32
    var generation: UInt16
}

let ownBuffer: UInt16 = 0xFFFF
/// Listening sockets on the port (SO_REUSEPORT). Each has its own listen queue, capped at
/// net.core.somaxconn, so a burst of new connections waits in eight queues instead of one.
let listenerCount = 8
let maxRings = 16
/// Each pass waits on ring 0 for this many completions or `waitMicroseconds`, whichever comes first.
let waitMinimum: UInt32 = 256
let waitMicroseconds = 1000

/// One io_uring with its own file table, connection slots and receive buffers.
final class Ring {
    let index: Int32
    let ring: UnsafeMutablePointer<io_uring>
    let slots: Int
    var table: UnsafeMutablePointer<Slot>
    var openCount = 0
    var accepting = false
    /// An accept found the file table full: accepting must move to another ring.
    var acceptFailed = false
    var acceptGeneration: UInt16 = 0
    /// Listeners whose multishot accept ended while this ring should accept; re-armed after the pass.
    var acceptRearm: [Int] = []

    let bufferCount: Int32 = 1024
    let bufferSize = 2048
    let bufferMemory: UnsafeMutablePointer<UInt8>
    let bufferRing: UnsafeMutablePointer<io_uring_buf_ring>
    let bufferMask: Int32
    var buffersReturned: Int32 = 0
    var rearm: [UInt32] = []

    let maxCompletions = 8192
    let cqes: UnsafeMutablePointer<UnsafeMutablePointer<io_uring_cqe>?>
    var pending = 0

    init?(index: Int32, slots: Int) {
        self.index = index
        self.slots = slots
        ring = .allocate(capacity: 1)
        guard tu_setup(ring, 4096, 16384) == 0 else { return nil }
        guard io_uring_register_files_sparse(ring, UInt32(slots)) == 0 else { return nil }
        bufferMemory = .allocate(capacity: Int(bufferCount) &* bufferSize)
        guard let br = tu_buffers(ring, UInt32(bufferCount), 0) else { return nil }
        bufferRing = br
        bufferMask = tu_buffer_mask(UInt32(bufferCount))
        for bid in 0..<bufferCount {
            tu_buffer_put(br, bufferMemory + Int(bid) &* bufferSize, UInt32(bufferSize), UInt16(bid), bufferMask, bid)
        }
        tu_buffer_commit(br, bufferCount)
        // A zeroed Slot is the default Slot(), so calloc gives a valid table whose pages the kernel
        // only maps when a connection lands in them.
        table = calloc(slots, MemoryLayout<Slot>.stride).unsafelyUnwrapped.bindMemory(to: Slot.self, capacity: slots)
        cqes = .allocate(capacity: maxCompletions)
    }

    func startAccepting(_ listeners: [Int32]) {
        accepting = true
        acceptFailed = false
        acceptGeneration &+= 1
        acceptRearm.removeAll(keepingCapacity: true)
        for (i, fd) in listeners.enumerated() {
            tu_accept(ring, fd, tag(.accept, slot: UInt32(i), generation: acceptGeneration))
        }
    }

    func stopAccepting(_ listeners: [Int32]) {
        accepting = false
        acceptFailed = false
        acceptRearm.removeAll(keepingCapacity: true)
        for i in 0..<listeners.count {
            tu_cancel(ring, tag(.accept, slot: UInt32(i), generation: acceptGeneration), tag(.close))
        }
    }

    @inline(__always)
    func returnBuffer(_ bid: UInt16) {
        tu_buffer_put(bufferRing, bufferMemory + Int(bid) &* bufferSize, UInt32(bufferSize), bid, bufferMask,
                      buffersReturned)
        buffersReturned &+= 1
    }

    func armReceive(_ slot: UInt32) {
        let s = table + Int(slot)
        s.pointee.receiving = true
        tu_recv(ring, slot, 0, tag(.recv, slot: slot, generation: s.pointee.generation))
    }
}

final class Server {
    let service: FeedService
    let listeners: [Int32]
    let slots: Int
    /// Accepting moves to another ring this many slots before a ring is full: accepts already in
    /// flight when it moves still land, and an accept that finds no free slot drops the connection.
    let margin: Int
    var rings: [Ring] = []
    var acceptor = -1
    var lastRingAttempt: Int64 = 0

    var arenas: [Arena] = []
    var responses: UnsafeMutablePointer<Response>
    var responseCount = 0
    var responseCapacity = 8192
    var parked: [Parked] = []

    init?(service: FeedService, listeners: [Int32], slots: Int) {
        self.service = service
        self.listeners = listeners
        self.slots = slots
        margin = min(1024, max(slots / 8, 16))
        responses = .allocate(capacity: responseCapacity)
        arenas.append(Arena())
        guard let first = Ring(index: 0, slots: slots) else { return nil }
        rings.append(first)
        acceptor = 0
        first.startAccepting(listeners)
    }

    func run() -> Never {
        while true {
            // Submit what the last pass queued and collect completions. Deferred task work only runs
            // inside an enter, so the other rings submit and collect without waiting, and ring 0 goes
            // last and waits. A pass then spans up to `waitMicroseconds` without spinning.
            var i = rings.count &- 1
            while i > 0 {
                _ = tu_poll(rings[i].ring)
                i &-= 1
            }
            var idle = rings.count == 1 && rings[0].openCount == 0 && rings[0].accepting
            if idle { for a in arenas where a.pendingSends > 0 { idle = false } }
            _ = tu_wait(rings[0].ring, idle ? 1 : waitMinimum, idle ? 0 : waitMicroseconds)
            pass()
        }
    }

    func freeArena() -> Int {
        for i in 0..<arenas.count where arenas[i].pendingSends == 0 { return i }
        arenas.append(Arena())
        return arenas.count &- 1
    }

    func pass() {
        var total = 0
        for r in rings {
            r.pending = Int(io_uring_peek_batch_cqe(r.ring, r.cqes, UInt32(r.maxCompletions)))
            total &+= r.pending
        }
        if total == 0 && parked.isEmpty { return }
        let arenaIndex = freeArena()
        let arena = arenas[arenaIndex]
        arena.out.truncate(0)
        responseCount = 0

        // Writes parked by a checkpoint hold go first, ahead of anything that arrived behind them.
        if !parked.isEmpty && !service.writesHeld { releaseParked(arena) }

        for r in rings where r.pending > 0 {
            for i in 0..<r.pending {
                let c = r.cqes[i].unsafelyUnwrapped
                let data = c.pointee.user_data
                let res = c.pointee.res
                let flags = c.pointee.flags
                let slot = UInt32(truncatingIfNeeded: (data >> 24) & 0xFF_FFFF)
                let generation = UInt16(truncatingIfNeeded: data >> 8)
                switch data & 0xFF {
                case Op.accept.rawValue:
                    if res >= 0 { opened(r, UInt32(res)) }
                    if flags & UInt32(TU_CQE_F_MORE) == 0 && r.accepting && generation == r.acceptGeneration {
                        if res == -ENFILE || res == -EMFILE {
                            r.acceptFailed = true // table full: accepting moves on below
                        } else {
                            r.acceptRearm.append(Int(slot))
                        }
                    }
                case Op.recv.rawValue:
                    received(r, slot, generation, res, flags, arena)
                case Op.send.rawValue:
                    sent(r, slot, generation, res, UInt16(truncatingIfNeeded: data >> 48))
                default:
                    break
                }
            }
        }

        if !service.endPass() { failBatch(arena) }
        for k in 0..<responseCount {
            let resp = responses[k]
            send(rings[Int(resp.ring)], resp.slot, arena.out.base + resp.start, resp.end &- resp.start,
                 arenaIndex: UInt16(arenaIndex), close: resp.close)
        }
        for r in rings {
            if r.pending > 0 { io_uring_cq_advance(r.ring, UInt32(r.pending)) }
            r.pending = 0
            let returned = Int(r.buffersReturned)
            if r.buffersReturned > 0 {
                tu_buffer_commit(r.bufferRing, r.buffersReturned)
                r.buffersReturned = 0
            }
            if !r.rearm.isEmpty { rearmReceives(r, returned) }
            if r.accepting && !r.acceptRearm.isEmpty {
                for l in r.acceptRearm {
                    tu_accept(r.ring, listeners[l], tag(.accept, slot: UInt32(l), generation: r.acceptGeneration))
                }
                r.acceptRearm.removeAll(keepingCapacity: true)
            }
        }
        service.tick()
        manageAccepting()
    }

    /// Re-arms receives that ran out of buffers, but only about as many as there are buffers to
    /// receive into: re-arming them all at once under overload just repeats ENOBUFS.
    func rearmReceives(_ r: Ring, _ returned: Int) {
        let budget = min(max(returned, 32), 2048)
        var armed = 0
        var kept = 0
        for i in 0..<r.rearm.count {
            let s = r.rearm[i]
            guard r.table[Int(s)].open && !r.table[Int(s)].receiving else { continue }
            if armed < budget {
                r.armReceive(s)
                armed &+= 1
            } else {
                r.rearm[kept] = s
                kept &+= 1
            }
        }
        r.rearm.removeLast(r.rearm.count &- kept)
    }

    /// Keeps exactly one ring accepting: the current one until it is `margin` slots from full, then
    /// the first ring with room for twice that, adding a ring when none has.
    func manageAccepting() {
        var skip = -1
        if acceptor >= 0 {
            let r = rings[acceptor]
            if r.acceptFailed { skip = acceptor }
            if r.accepting && !r.acceptFailed && r.openCount < r.slots &- margin { return }
            if r.accepting { r.stopAccepting(listeners) }
            acceptor = -1
        }
        for r in rings where r.openCount < r.slots &- 2 &* margin && Int(r.index) != skip {
            acceptor = Int(r.index)
            r.startAccepting(listeners)
            return
        }
        guard rings.count < maxRings else { return }
        // A ring that can't be created (memory) is retried once a second.
        let now = sv_monotonic_s()
        guard now != lastRingAttempt else { return }
        lastRingAttempt = now
        guard let r = Ring(index: Int32(rings.count), slots: slots) else { return }
        rings.append(r)
        acceptor = Int(r.index)
        r.startAccepting(listeners)
    }

    // MARK: Completions

    func opened(_ r: Ring, _ slot: UInt32) {
        let s = r.table + Int(slot)
        let generation = s.pointee.generation &+ 1
        s.pointee = Slot()
        s.pointee.generation = generation
        s.pointee.open = true
        r.openCount &+= 1
        r.armReceive(slot)
    }

    func received(_ r: Ring, _ slot: UInt32, _ generation: UInt16, _ res: Int32, _ flags: UInt32, _ arena: Arena) {
        let s = r.table + Int(slot)
        let hasBuffer = flags & UInt32(TU_CQE_F_BUFFER) != 0
        let bid = UInt16(truncatingIfNeeded: flags >> UInt32(TU_CQE_BUFFER_SHIFT))
        let current = s.pointee.open && s.pointee.generation == generation
        if flags & UInt32(TU_CQE_F_MORE) == 0 && current { s.pointee.receiving = false }
        guard current else {
            if hasBuffer { r.returnBuffer(bid) }
            return
        }
        if res <= 0 {
            if hasBuffer { r.returnBuffer(bid) }
            if res == -ENOBUFS {
                r.rearm.append(slot)
            } else {
                closeSlot(r, slot)
            }
            return
        }
        let data = UnsafePointer(r.bufferMemory + Int(bid) &* r.bufferSize)
        if s.pointee.parked {
            appendInput(s, data, Int(res))
            r.returnBuffer(bid)
        } else if s.pointee.inputCount > 0 {
            appendInput(s, data, Int(res))
            r.returnBuffer(bid)
            let p = UnsafePointer(s.pointee.input.unsafelyUnwrapped)
            let n = Int(s.pointee.inputCount)
            s.pointee.inputCount = 0
            answer(r, slot, p, n, arena)
        } else {
            answer(r, slot, data, Int(res), arena)
            r.returnBuffer(bid)
        }
        if !s.pointee.receiving && s.pointee.open { r.rearm.append(slot) }
    }

    func releaseParked(_ arena: Arena) {
        let list = parked
        parked.removeAll(keepingCapacity: true)
        for e in list {
            let r = rings[Int(e.ring)]
            let s = r.table + Int(e.slot)
            guard s.pointee.open && s.pointee.generation == e.generation && s.pointee.parked else { continue }
            s.pointee.parked = false
            let n = Int(s.pointee.inputCount)
            s.pointee.inputCount = 0
            if n > 0 { answer(r, e.slot, UnsafePointer(s.pointee.input.unsafelyUnwrapped), n, arena) }
        }
    }

    func appendInput(_ s: UnsafeMutablePointer<Slot>, _ p: UnsafePointer<UInt8>, _ n: Int) {
        let used = Int(s.pointee.inputCount)
        if Int(s.pointee.inputCapacity) < used &+ n {
            let cap = max((used &+ n) &* 2, 4096)
            let fresh = UnsafeMutablePointer<UInt8>.allocate(capacity: cap)
            if used > 0 { fresh.update(from: s.pointee.input.unsafelyUnwrapped, count: used) }
            s.pointee.input?.deallocate()
            s.pointee.input = fresh
            s.pointee.inputCapacity = Int32(cap)
        }
        (s.pointee.input.unsafelyUnwrapped + used).update(from: p, count: n)
        s.pointee.inputCount = Int32(used &+ n)
    }

    /// Keeps the unparsed tail of `p`. It may lie in the slot's own input buffer (whose count the
    /// caller has already reset); anything else is copied in.
    func keepTail(_ s: UnsafeMutablePointer<Slot>, _ p: UnsafePointer<UInt8>, _ rest: Int) {
        if let input = s.pointee.input, p >= UnsafePointer(input), p < UnsafePointer(input) + Int(s.pointee.inputCapacity) {
            memmove(input, p, rest)
            s.pointee.inputCount = Int32(rest)
        } else {
            s.pointee.inputCount = 0
            appendInput(s, p, rest)
        }
    }

    /// Parses every complete request in `p` and appends the responses to the arena.
    func answer(_ r: Ring, _ slot: UInt32, _ p: UnsafePointer<UInt8>, _ n: Int, _ arena: Arena) {
        let s = r.table + Int(slot)
        var offset = 0
        let start = arena.out.count
        var close = false
        let crowded = r.openCount > r.slots &- 256
        loop: while offset < n {
            switch parseRequest(ByteView(base: p + offset, count: n &- offset)) {
            case .incomplete:
                keepTail(s, p + offset, n &- offset)
                break loop
            case .invalid:
                appendBadRequest(into: &arena.out)
                close = true
                break loop
            case .request(let request, let length):
                if request.method == .post && service.writesHeld {
                    // Wait out the checkpoint hold with this request and everything behind it unread.
                    keepTail(s, p + offset, n &- offset)
                    s.pointee.parked = true
                    parked.append(Parked(ring: r.index, slot: slot, generation: s.pointee.generation))
                    break loop
                }
                let closeThis = !request.keepAlive || crowded
                service.appendHTTPResponse(request, close: closeThis, into: &arena.out)
                offset &+= length
                if closeThis {
                    close = true
                    break loop
                }
            }
        }
        // An idle connection keeps no input memory: free the buffer once nothing waits in it.
        if s.pointee.inputCount == 0, let input = s.pointee.input {
            input.deallocate()
            s.pointee.input = nil
            s.pointee.inputCapacity = 0
        }
        if arena.out.count > start || close {
            if responseCount == responseCapacity {
                let bigger = UnsafeMutablePointer<Response>.allocate(capacity: responseCapacity &* 2)
                bigger.update(from: responses, count: responseCount)
                responses.deallocate()
                responses = bigger
                responseCapacity &*= 2
            }
            responses[responseCount] = Response(ring: r.index, slot: slot, start: start, end: arena.out.count, close: close)
            responseCount &+= 1
        }
    }

    func failBatch(_ arena: Arena) {
        arena.out.truncate(0)
        for k in 0..<responseCount {
            let start = arena.out.count
            appendInternalError(close: true, into: &arena.out)
            responses[k].start = start
            responses[k].end = arena.out.count
            responses[k].close = true
        }
    }

    // MARK: Sending

    func send(_ r: Ring, _ slot: UInt32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int, arenaIndex: UInt16, close: Bool) {
        let s = r.table + Int(slot)
        guard s.pointee.open else { return }
        if close { s.pointee.closeAfterSend = true }
        if n == 0 {
            if close && !s.pointee.sending { closeSlot(r, slot) }
            return
        }
        if s.pointee.sending {
            appendOverflow(s, p, n)
            return
        }
        submitSend(r, slot, p, n, arenaIndex)
    }

    func submitSend(_ r: Ring, _ slot: UInt32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int, _ arenaIndex: UInt16) {
        let s = r.table + Int(slot)
        s.pointee.sending = true
        s.pointee.sendArena = arenaIndex
        s.pointee.sendPointer = p
        s.pointee.sendRemaining = Int32(n)
        if arenaIndex != ownBuffer { arenas[Int(arenaIndex)].pendingSends &+= 1 }
        tu_send(r.ring, slot, p, UInt32(n), tag(.send, slot: slot, generation: s.pointee.generation, arena: arenaIndex))
    }

    func appendOverflow(_ s: UnsafeMutablePointer<Slot>, _ p: UnsafeMutablePointer<UInt8>, _ n: Int) {
        let used = Int(s.pointee.overflowCount)
        if Int(s.pointee.overflowCapacity) < used &+ n {
            let cap = max((used &+ n) &* 2, 4096)
            let fresh = UnsafeMutablePointer<UInt8>.allocate(capacity: cap)
            if used > 0 { fresh.update(from: s.pointee.overflow.unsafelyUnwrapped, count: used) }
            s.pointee.overflow?.deallocate()
            s.pointee.overflow = fresh
            s.pointee.overflowCapacity = Int32(cap)
        }
        (s.pointee.overflow.unsafelyUnwrapped + used).update(from: p, count: n)
        s.pointee.overflowCount = Int32(used &+ n)
    }

    func sent(_ r: Ring, _ slot: UInt32, _ generation: UInt16, _ res: Int32, _ arenaIndex: UInt16) {
        let s = r.table + Int(slot)
        let current = s.pointee.open && s.pointee.generation == generation
        if arenaIndex != ownBuffer { arenas[Int(arenaIndex)].pendingSends &-= 1 }
        guard current else { return }
        if res < 0 {
            s.pointee.sending = false
            closeSlot(r, slot)
            return
        }
        if res < s.pointee.sendRemaining {
            // Partial send: the memory is still owned until this send finishes.
            let p = s.pointee.sendPointer.unsafelyUnwrapped + Int(res)
            let rest = Int(s.pointee.sendRemaining &- res)
            s.pointee.sending = false
            submitSend(r, slot, p, rest, arenaIndex)
            return
        }
        s.pointee.sending = false
        if arenaIndex == ownBuffer, let owned = s.pointee.sendInFlightOwned {
            owned.deallocate()
            s.pointee.sendInFlightOwned = nil
        }
        if s.pointee.overflowCount > 0 {
            // Move queued bytes to a fresh buffer so new overflow can't touch memory in flight.
            let n = Int(s.pointee.overflowCount)
            let p = s.pointee.overflow.unsafelyUnwrapped
            s.pointee.overflow = nil
            s.pointee.overflowCount = 0
            s.pointee.overflowCapacity = 0
            s.pointee.sendInFlightOwned = p
            submitSend(r, slot, p, n, ownBuffer)
            return
        }
        if s.pointee.closeAfterSend { closeSlot(r, slot) }
    }

    // MARK: Closing

    func closeSlot(_ r: Ring, _ slot: UInt32) {
        let s = r.table + Int(slot)
        guard s.pointee.open else { return }
        if s.pointee.sending {
            // Close once the send in flight completes.
            s.pointee.closeAfterSend = true
            return
        }
        s.pointee.input?.deallocate()
        s.pointee.overflow?.deallocate()
        s.pointee.sendInFlightOwned?.deallocate()
        let generation = s.pointee.generation
        s.pointee = Slot()
        s.pointee.generation = generation
        r.openCount &-= 1
        tu_close(r.ring, slot, tag(.close, slot: slot))
    }
}
#endif
