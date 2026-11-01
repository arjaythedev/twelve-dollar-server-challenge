// Fallback for kernels or sandboxes without io_uring: the single-threaded epoll server.
//
// One loop iteration is one batch: read every ready socket into one input arena, parse every complete
// request in place, answer them all inside one SQLite transaction, commit, and only then write the
// responses. Reads inside the batch therefore never leak uncommitted data: if the commit fails, every
// response of the batch becomes a 500.

#if os(Linux)
import CShim

/// Per-descriptor state. Idle connections own no buffers.
struct Connection {
    var open = false
    /// Bytes of a request that hasn't fully arrived.
    var input: UnsafeMutablePointer<UInt8>? = nil
    var inputCount: Int32 = 0
    var inputCapacity: Int32 = 0
    /// Response bytes the socket didn't accept yet.
    var output: UnsafeMutablePointer<UInt8>? = nil
    var outputCount: Int32 = 0
    var outputCapacity: Int32 = 0
    var outputOffset: Int32 = 0
    var closeAfterFlush = false
    var writableArmed = false
    /// Least recently active first; used to shed idle connections at the descriptor limit.
    var lruPrev: Int32 = -1
    var lruNext: Int32 = -1
}

/// A connection's responses in this batch: `outputStart..<outputEnd` of the output arena.
struct BatchEntry {
    var fd: Int32
    var outputStart: Int
    var outputEnd: Int
    var close: Bool
}

struct EpollServer: ~Copyable {
    let service: FeedService
    let listener: Int32
    let epoll: Int32
    let maxConnections: Int
    /// Above this many open connections, responses carry `Connection: close`.
    let softLimit: Int
    /// At this many, the least recently active idle connection is closed before accepting another.
    let hardLimit: Int

    let connections: UnsafeMutablePointer<Connection>
    var openCount = 0
    var lruHead: Int32 = -1
    var lruTail: Int32 = -1

    var inputArena: OutBuffer
    var outputArena: OutBuffer
    let batch: UnsafeMutablePointer<BatchEntry>
    var batchCount = 0

    let eventFDs: UnsafeMutablePointer<Int32>
    let eventFlags: UnsafeMutablePointer<UInt32>
    let maxEvents = 1024
    /// Per event: offset of the connection's bytes in the input arena, or -1.
    let readStart: UnsafeMutablePointer<Int>
    let readEnd: UnsafeMutablePointer<Int>

    init?(service: FeedService, host: UnsafePointer<CChar>, port: Int32) {
        sv_ignore_sigpipe()
        let limit = Int(sv_raise_nofile())
        let fd = sv_listen(host, port, 4096)
        guard fd >= 0 else { return nil }
        self.service = service
        listener = fd
        epoll = sv_epoll_create()
        maxConnections = limit
        softLimit = max(limit &- 1024, limit &* 9 / 10)
        hardLimit = limit &- 64
        connections = .allocate(capacity: limit)
        connections.initialize(repeating: Connection(), count: limit)
        inputArena = OutBuffer(capacity: 4 << 20)
        outputArena = OutBuffer(capacity: 4 << 20)
        batch = .allocate(capacity: maxEvents)
        eventFDs = .allocate(capacity: maxEvents)
        eventFlags = .allocate(capacity: maxEvents)
        readStart = .allocate(capacity: maxEvents)
        readEnd = .allocate(capacity: maxEvents)
        _ = sv_epoll_add(epoll, listener, 0)
    }

    mutating func run() -> Never {
        while true {
            let n = Int(sv_epoll_wait(epoll, eventFDs, eventFlags, Int32(maxEvents), -1))
            if n <= 0 { continue }
            runBatch(n)
        }
    }

    mutating func runBatch(_ n: Int) {
        inputArena.truncate(0)
        outputArena.truncate(0)
        batchCount = 0
        var acceptPending = false

        // 1. Read every readable connection into the arena. Views into it stay valid for the batch.
        for i in 0..<n {
            let fd = eventFDs[i]
            readStart[i] = -1
            if fd == listener {
                acceptPending = true
                continue
            }
            let flags = eventFlags[i]
            if flags & 2 != 0 { flush(fd) }
            if flags & 5 != 0 { readInto(i, fd) }
        }

        // 2. Parse and answer. One transaction when the batch has more than one request.
        var requests = 0
        for i in 0..<n where readStart[i] >= 0 {
            requests &+= countHint(i)
        }
        let transaction = requests > 1 && service.beginBatch()
        for i in 0..<n where readStart[i] >= 0 {
            answer(eventFDs[i], readStart[i], readEnd[i])
        }
        if transaction && !service.endBatch() {
            failBatch()
        }

        // 3. Send.
        for k in 0..<batchCount {
            let e = batch[k]
            send(e.fd, outputArena.base + e.outputStart, e.outputEnd &- e.outputStart, close: e.close)
        }

        // Commits only mark a checkpoint as due; run it after the responses are written.
        service.checkpointIfDue()
        if acceptPending { acceptAll() }
    }

    /// Whether this event carries at least one request; enough to decide on a transaction.
    @inline(__always)
    func countHint(_ i: Int) -> Int { readEnd[i] > readStart[i] ? 1 : 0 }

    // MARK: Reading

    mutating func readInto(_ i: Int, _ fd: Int32) {
        let c = connections + Int(fd)
        guard c.pointee.open else { return }
        let start = inputArena.count
        if c.pointee.inputCount > 0 {
            inputArena.append(UnsafePointer(c.pointee.input.unsafelyUnwrapped), Int(c.pointee.inputCount))
            c.pointee.inputCount = 0
        }
        inputArena.reserve(16384)
        let room = inputArena.capacity &- inputArena.count
        let got = read(fd, inputArena.base + inputArena.count, room)
        if got > 0 {
            inputArena.count &+= got
        } else if got == 0 || (got < 0 && sv_errno() != EAGAIN && sv_errno() != EINTR) {
            closeConnection(fd)
            inputArena.truncate(start)
            return
        }
        if inputArena.count > start {
            readStart[i] = start
            readEnd[i] = inputArena.count
            touch(fd)
        } else {
            inputArena.truncate(start)
        }
    }

    /// Parses the connection's bytes and appends its responses to the output arena.
    mutating func answer(_ fd: Int32, _ start: Int, _ end: Int) {
        let base = UnsafePointer(inputArena.base)
        var offset = start
        let outputStart = outputArena.count
        var close = false
        let crowded = openCount > softLimit
        while offset < end {
            let view = ByteView(base: base + offset, count: end &- offset)
            switch parseRequest(view) {
            case .incomplete:
                keepPartial(fd, base + offset, end &- offset)
                offset = end
            case .invalid:
                appendBadRequest(into: &outputArena)
                close = true
                offset = end
            case .request(let request, let length):
                let closeThis = !request.keepAlive || crowded
                service.appendHTTPResponse(request, close: closeThis, into: &outputArena)
                offset &+= length
                if closeThis {
                    close = true
                    offset = end
                }
            }
        }
        if outputArena.count > outputStart || close {
            batch[batchCount] = BatchEntry(fd: fd, outputStart: outputStart, outputEnd: outputArena.count, close: close)
            batchCount &+= 1
        }
    }

    mutating func keepPartial(_ fd: Int32, _ p: UnsafePointer<UInt8>, _ n: Int) {
        let c = connections + Int(fd)
        if Int(c.pointee.inputCapacity) < n {
            c.pointee.input?.deallocate()
            let cap = max(n, 2048)
            c.pointee.input = .allocate(capacity: cap)
            c.pointee.inputCapacity = Int32(cap)
        }
        c.pointee.input.unsafelyUnwrapped.update(from: p, count: n)
        c.pointee.inputCount = Int32(n)
    }

    /// The commit failed: nothing in this batch may claim success.
    mutating func failBatch() {
        outputArena.truncate(0)
        for k in 0..<batchCount {
            let start = outputArena.count
            appendInternalError(close: true, into: &outputArena)
            batch[k].outputStart = start
            batch[k].outputEnd = outputArena.count
            batch[k].close = true
        }
    }

    // MARK: Writing

    mutating func send(_ fd: Int32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int, close: Bool) {
        let c = connections + Int(fd)
        guard c.pointee.open else { return }
        if c.pointee.outputCount > 0 {
            // Earlier responses are still queued; keep order.
            queue(fd, p, n)
        } else if n > 0 {
            let sent = write(fd, p, n)
            if sent < 0 && sv_errno() != EAGAIN && sv_errno() != EINTR {
                closeConnection(fd)
                return
            }
            let done = max(sent, 0)
            if done < n { queue(fd, p + done, n &- done) }
        }
        if close {
            if c.pointee.outputCount > 0 { c.pointee.closeAfterFlush = true } else { closeConnection(fd) }
        }
    }

    mutating func queue(_ fd: Int32, _ p: UnsafeMutablePointer<UInt8>, _ n: Int) {
        let c = connections + Int(fd)
        let used = Int(c.pointee.outputCount)
        if Int(c.pointee.outputCapacity) < used &+ n {
            let cap = max((used &+ n) &* 2, 4096)
            let fresh = UnsafeMutablePointer<UInt8>.allocate(capacity: cap)
            if used > 0 { fresh.update(from: c.pointee.output.unsafelyUnwrapped, count: used) }
            c.pointee.output?.deallocate()
            c.pointee.output = fresh
            c.pointee.outputCapacity = Int32(cap)
        }
        (c.pointee.output.unsafelyUnwrapped + used).update(from: p, count: n)
        c.pointee.outputCount = Int32(used &+ n)
        if !c.pointee.writableArmed {
            c.pointee.writableArmed = true
            _ = sv_epoll_mod(epoll, fd, 1)
        }
    }

    mutating func flush(_ fd: Int32) {
        let c = connections + Int(fd)
        guard c.pointee.open, c.pointee.outputCount > 0 else { return }
        let off = Int(c.pointee.outputOffset)
        let left = Int(c.pointee.outputCount) &- off
        let sent = write(fd, c.pointee.output.unsafelyUnwrapped + off, left)
        if sent < 0 {
            if sv_errno() != EAGAIN && sv_errno() != EINTR { closeConnection(fd) }
            return
        }
        if sent < left {
            c.pointee.outputOffset = Int32(off &+ sent)
            return
        }
        c.pointee.outputCount = 0
        c.pointee.outputOffset = 0
        c.pointee.output?.deallocate()
        c.pointee.output = nil
        c.pointee.outputCapacity = 0
        c.pointee.writableArmed = false
        _ = sv_epoll_mod(epoll, fd, 0)
        if c.pointee.closeAfterFlush { closeConnection(fd) }
    }

    // MARK: Connections

    mutating func acceptAll() {
        while true {
            if openCount >= hardLimit && !evictIdle() { return }
            let fd = sv_accept(listener)
            if fd < 0 { return }
            if Int(fd) >= maxConnections {
                _ = CShim.close(fd)
                return
            }
            let c = connections + Int(fd)
            c.pointee = Connection()
            c.pointee.open = true
            openCount &+= 1
            lruAppend(fd)
            _ = sv_epoll_add(epoll, fd, 0)
        }
    }

    /// Closes the least recently active connection that has nothing in flight.
    mutating func evictIdle() -> Bool {
        var fd = lruHead
        var tries = 0
        while fd >= 0 && tries < 64 {
            let c = connections + Int(fd)
            if c.pointee.inputCount == 0 && c.pointee.outputCount == 0 {
                closeConnection(fd)
                return true
            }
            fd = c.pointee.lruNext
            tries &+= 1
        }
        return false
    }

    mutating func closeConnection(_ fd: Int32) {
        let c = connections + Int(fd)
        guard c.pointee.open else { return }
        lruRemove(fd)
        c.pointee.input?.deallocate()
        c.pointee.output?.deallocate()
        c.pointee = Connection()
        openCount &-= 1
        _ = CShim.close(fd)
    }

    @inline(__always)
    mutating func touch(_ fd: Int32) {
        if lruTail == fd { return }
        lruRemove(fd)
        lruAppend(fd)
    }

    mutating func lruAppend(_ fd: Int32) {
        let c = connections + Int(fd)
        c.pointee.lruPrev = lruTail
        c.pointee.lruNext = -1
        if lruTail >= 0 { connections[Int(lruTail)].lruNext = fd } else { lruHead = fd }
        lruTail = fd
    }

    mutating func lruRemove(_ fd: Int32) {
        let c = connections + Int(fd)
        let prev = c.pointee.lruPrev
        let next = c.pointee.lruNext
        if prev >= 0 { connections[Int(prev)].lruNext = next } else if lruHead == fd { lruHead = next }
        if next >= 0 { connections[Int(next)].lruPrev = prev } else if lruTail == fd { lruTail = prev }
        c.pointee.lruPrev = -1
        c.pointee.lruNext = -1
    }
}
#endif
