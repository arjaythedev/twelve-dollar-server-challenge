// io_uring HTTP/1.1 server around the feed service.
//
// Each worker owns one ring with its own registered file table, so accepted sockets live in the ring
// (direct descriptors) instead of the process descriptor table. One wait returns a batch of
// completions: receives are parsed in place inside the kernel-provided buffers, every request of the
// batch is answered inside one SQLite transaction, and the sends go out only after the commit.
// Responses are written into an arena that stays alive until its last send completes, so nothing is
// copied between building a response and sending it.

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
    var sendArena: UInt16 = 0
    var sendPointer: UnsafeMutablePointer<UInt8>? = nil
    var sendRemaining: Int32 = 0
    var closeAfterSend = false
    /// Bytes of a request that hasn't fully arrived.
    var input: UnsafeMutablePointer<UInt8>? = nil
    var inputCount: Int32 = 0
    var inputCapacity: Int32 = 0
    /// Responses produced while a send was in flight; sent when it completes.
    var overflow: UnsafeMutablePointer<UInt8>? = nil
    var overflowCount: Int32 = 0
    var overflowCapacity: Int32 = 0
    /// A slot-owned buffer currently being sent (former overflow).
    var sendInFlightOwned: UnsafeMutablePointer<UInt8>? = nil
}

/// Output memory for one batch. Reused once every send that points into it has completed.
final class Arena {
    var out = OutBuffer(capacity: 1 << 20)
    var pendingSends = 0
}

struct Response {
    var slot: UInt32
    var start: Int
    var end: Int
    var close: Bool
}

let ownBuffer: UInt16 = 0xFFFF

final class Worker {
    let service: FeedService
    let ring: UnsafeMutablePointer<io_uring>
    let listener: Int32
    let slots: Int
    let softLimit: Int
    let acceptStop: Int
    let acceptResume: Int
    var table: UnsafeMutablePointer<Slot>
    var openCount = 0
    var accepting = false

    let bufferCount: Int32 = 4096
    let bufferSize = 2048
    let bufferMemory: UnsafeMutablePointer<UInt8>
    let bufferRing: UnsafeMutablePointer<io_uring_buf_ring>
    let bufferMask: Int32
    var buffersReturned: Int32 = 0
    var rearm: [UInt32] = []

    var arenas: [Arena] = []
    var responses: UnsafeMutablePointer<Response>
    var responseCount = 0
    let maxCompletions = 4096
    let cqes: UnsafeMutablePointer<UnsafeMutablePointer<io_uring_cqe>?>
    var busy = false

    init?(service: FeedService, listener: Int32, slots: Int) {
        self.service = service
        self.listener = listener
        self.slots = slots
        // A ring that is nearly full stops accepting, so the kernel hands new connections to the
        // next ring's accept. Without this every connection lands on the first ring.
        acceptStop = slots &- 1024
        acceptResume = slots &- 2048
        softLimit = slots &- 256
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
        // only maps when a connection lands in them. Rings that never fill cost almost nothing.
        table = calloc(slots, MemoryLayout<Slot>.stride).unsafelyUnwrapped.bindMemory(to: Slot.self, capacity: slots)
        responses = .allocate(capacity: maxCompletions)
        cqes = .allocate(capacity: maxCompletions)
        arenas.append(Arena())
        startAccepting()
    }

    var acceptGeneration: UInt16 = 0

    func startAccepting() {
        accepting = true
        acceptGeneration &+= 1
        tu_accept(ring, listener, tag(.accept, generation: acceptGeneration))
    }

    func stopAccepting() {
        accepting = false
        tu_cancel(ring, tag(.accept, generation: acceptGeneration), tag(.close))
    }

    /// With connections open, wait until 32 completions are ready or 250 µs pass, so one wakeup handles
    /// a batch. With none open, block until the first completion.
    func run() -> Never {
        while true {
            if openCount > 0 {
                _ = tu_wait(ring, 32, 250)
            } else {
                _ = tu_wait(ring, 1, 0)
            }
            let n = Int(io_uring_peek_batch_cqe(ring, cqes, UInt32(maxCompletions)))
            if n > 0 { process(n) }
        }
    }

    func freeArena() -> Int {
        for i in 0..<arenas.count where arenas[i].pendingSends == 0 { return i }
        arenas.append(Arena())
        return arenas.count &- 1
    }

    func process(_ n: Int) {
        // Count requests first: one transaction only pays off for more than one.
        var receives = 0
        for i in 0..<n {
            let c = cqes[i].unsafelyUnwrapped
            if c.pointee.user_data & 0xFF == Op.recv.rawValue && c.pointee.res > 0 { receives &+= 1 }
        }
        let arenaIndex = freeArena()
        let arena = arenas[arenaIndex]
        arena.out.truncate(0)
        responseCount = 0
        let transaction = receives > 1
        if transaction { tu_lock(); _ = service.beginBatch() } else if receives == 1 { tu_lock() }

        for i in 0..<n {
            let c = cqes[i].unsafelyUnwrapped
            let data = c.pointee.user_data
            let res = c.pointee.res
            let flags = c.pointee.flags
            let slot = UInt32(truncatingIfNeeded: (data >> 24) & 0xFF_FFFF)
            let generation = UInt16(truncatingIfNeeded: data >> 8)
            switch data & 0xFF {
            case Op.accept.rawValue:
                if res >= 0 { opened(UInt32(res)) }
                if flags & UInt32(TU_CQE_F_MORE) == 0 && generation == acceptGeneration { accepting = false }
            case Op.recv.rawValue:
                received(slot, generation, res, flags, arena)
            case Op.send.rawValue:
                sent(slot, generation, res, UInt16(truncatingIfNeeded: data >> 48))
            default:
                break
            }
        }

        if receives > 0 {
            if transaction && !service.endBatch() { failBatch(arena) }
            tu_unlock()
        }
        for k in 0..<responseCount {
            let r = responses[k]
            send(r.slot, arena.out.base + r.start, r.end &- r.start, arenaIndex: UInt16(arenaIndex), close: r.close)
        }
        io_uring_cq_advance(ring, UInt32(n))
        let returned = Int(buffersReturned)
        if buffersReturned > 0 {
            tu_buffer_commit(bufferRing, buffersReturned)
            buffersReturned = 0
        }
        if !rearm.isEmpty {
            // Re-arm receives that ran out of buffers, but only about as many as there are buffers
            // to receive into: re-arming them all at once under overload just repeats ENOBUFS.
            let budget = min(max(returned, 32), 2048)
            var armed = 0
            var kept = 0
            for i in 0..<rearm.count {
                let s = rearm[i]
                guard table[Int(s)].open && !table[Int(s)].receiving else { continue }
                if armed < budget {
                    armReceive(s)
                    armed &+= 1
                } else {
                    rearm[kept] = s
                    kept &+= 1
                }
            }
            rearm.removeLast(rearm.count &- kept)
        }
        // Responses are queued; submit them, then run a due checkpoint behind them.
        if service.checkpointDue {
            _ = io_uring_submit(ring)
            tu_lock()
            service.checkpointIfDue()
            tu_unlock()
        }
        if accepting && openCount >= acceptStop {
            stopAccepting()
        } else if !accepting && openCount < acceptResume {
            startAccepting()
        }
    }

    // MARK: Completions

    func opened(_ slot: UInt32) {
        let s = table + Int(slot)
        let generation = s.pointee.generation &+ 1
        s.pointee = Slot()
        s.pointee.generation = generation
        s.pointee.open = true
        openCount &+= 1
        armReceive(slot)
    }

    func armReceive(_ slot: UInt32) {
        let s = table + Int(slot)
        s.pointee.receiving = true
        tu_recv(ring, slot, 0, tag(.recv, slot: slot, generation: s.pointee.generation))
    }

    @inline(__always)
    func returnBuffer(_ bid: UInt16) {
        tu_buffer_put(bufferRing, bufferMemory + Int(bid) &* bufferSize, UInt32(bufferSize), bid, bufferMask,
                      buffersReturned)
        buffersReturned &+= 1
    }

    func received(_ slot: UInt32, _ generation: UInt16, _ res: Int32, _ flags: UInt32, _ arena: Arena) {
        let s = table + Int(slot)
        let hasBuffer = flags & UInt32(TU_CQE_F_BUFFER) != 0
        let bid = UInt16(truncatingIfNeeded: flags >> UInt32(TU_CQE_BUFFER_SHIFT))
        let current = s.pointee.open && s.pointee.generation == generation
        if flags & UInt32(TU_CQE_F_MORE) == 0 && current { s.pointee.receiving = false }
        guard current else {
            if hasBuffer { returnBuffer(bid) }
            return
        }
        if res <= 0 {
            if hasBuffer { returnBuffer(bid) }
            if res == -ENOBUFS {
                rearm.append(slot)
            } else {
                closeSlot(slot)
            }
            return
        }
        let data = UnsafePointer(bufferMemory + Int(bid) &* bufferSize)
        if s.pointee.inputCount > 0 {
            appendInput(s, data, Int(res))
            returnBuffer(bid)
            let p = UnsafePointer(s.pointee.input.unsafelyUnwrapped)
            let n = Int(s.pointee.inputCount)
            s.pointee.inputCount = 0
            answer(slot, p, n, arena)
        } else {
            answer(slot, data, Int(res), arena)
            returnBuffer(bid)
        }
        if !s.pointee.receiving && s.pointee.open { rearm.append(slot) }
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

    /// Parses every complete request in `p` and appends the responses to the arena.
    func answer(_ slot: UInt32, _ p: UnsafePointer<UInt8>, _ n: Int, _ arena: Arena) {
        let s = table + Int(slot)
        var offset = 0
        let start = arena.out.count
        var close = false
        let crowded = openCount > softLimit
        while offset < n {
            switch parseRequest(ByteView(base: p + offset, count: n &- offset)) {
            case .incomplete:
                // Keep the tail; it may alias the slot's own input buffer, so stage it first.
                let rest = n &- offset
                if s.pointee.input != nil && UnsafePointer(s.pointee.input.unsafelyUnwrapped) == p {
                    s.pointee.input.unsafelyUnwrapped.update(from: p + offset, count: rest)
                    s.pointee.inputCount = Int32(rest)
                } else {
                    appendInput(s, p + offset, rest)
                }
                offset = n
            case .invalid:
                appendBadRequest(into: &arena.out)
                close = true
                offset = n
            case .request(let request, let length):
                let closeThis = !request.keepAlive || crowded
                service.appendHTTPResponse(request, close: closeThis, into: &arena.out)
                offset &+= length
                if closeThis {
                    close = true
                    offset = n
                }
            }
        }
        if arena.out.count > start || close {
            responses[responseCount] = Response(slot: slot, start: start, end: arena.out.count, close: close)
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

    func send(_ slot: UInt32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int, arenaIndex: UInt16, close: Bool) {
        let s = table + Int(slot)
        guard s.pointee.open else { return }
        if close { s.pointee.closeAfterSend = true }
        if n == 0 {
            if close && !s.pointee.sending { closeSlot(slot) }
            return
        }
        if s.pointee.sending {
            appendOverflow(s, p, n)
            return
        }
        submitSend(slot, p, n, arenaIndex)
    }

    func submitSend(_ slot: UInt32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int, _ arenaIndex: UInt16) {
        let s = table + Int(slot)
        s.pointee.sending = true
        s.pointee.sendArena = arenaIndex
        s.pointee.sendPointer = p
        s.pointee.sendRemaining = Int32(n)
        if arenaIndex != ownBuffer { arenas[Int(arenaIndex)].pendingSends &+= 1 }
        tu_send(ring, slot, p, UInt32(n), tag(.send, slot: slot, generation: s.pointee.generation, arena: arenaIndex))
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

    func sent(_ slot: UInt32, _ generation: UInt16, _ res: Int32, _ arenaIndex: UInt16) {
        let s = table + Int(slot)
        let current = s.pointee.open && s.pointee.generation == generation
        if arenaIndex != ownBuffer { arenas[Int(arenaIndex)].pendingSends &-= 1 }
        guard current else { return }
        if res < 0 {
            s.pointee.sending = false
            closeSlot(slot)
            return
        }
        if res < s.pointee.sendRemaining {
            // Partial send: the memory is still owned until this send finishes.
            let p = s.pointee.sendPointer.unsafelyUnwrapped + Int(res)
            let rest = Int(s.pointee.sendRemaining &- res)
            s.pointee.sending = false
            submitSend(slot, p, rest, arenaIndex)
            return
        }
        s.pointee.sending = false
        if s.pointee.overflowCount > 0 {
            // Move queued bytes to a fresh buffer so new overflow can't touch memory in flight.
            let n = Int(s.pointee.overflowCount)
            let p = s.pointee.overflow.unsafelyUnwrapped
            s.pointee.overflow = nil
            s.pointee.overflowCount = 0
            s.pointee.overflowCapacity = 0
            ownedInFlight(slot, p, n)
            return
        }
        if s.pointee.closeAfterSend { closeSlot(slot) }
    }

    /// Sends a buffer this slot owns; it is freed when the slot closes or the next overflow replaces it.
    func ownedInFlight(_ slot: UInt32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int) {
        let s = table + Int(slot)
        s.pointee.sendInFlightOwned?.deallocate()
        s.pointee.sendInFlightOwned = p
        submitSend(slot, p, n, ownBuffer)
    }

    // MARK: Closing

    func closeSlot(_ slot: UInt32) {
        let s = table + Int(slot)
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
        openCount &-= 1
        tu_close(ring, slot, tag(.close, slot: slot))
    }
}
#endif
