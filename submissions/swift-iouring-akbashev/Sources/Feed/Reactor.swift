// The reactor: the server's one thread (the main actor's, like everything here), driving io_uring rings. Connections live in the
// rings' tables of direct descriptors, not in the process's descriptor table, so the box's 65,535-descriptor limit
// caps one table, not the server: when a ring's table is nearly full, accepting moves to another ring.
//
// Each pass of the loop submits what the last one prepared and takes every ring's completions: new connections,
// received requests (parsed in place in the kernel's buffers and answered from SQLite inside the pass's transaction),
// finished sends. At the end of the pass the transaction commits, and only then do the pass's responses go out
// (rule 6: a 201 is committed). Checkpoints are the checkpointer's (Checkpointer.swift): the loop starts one without
// waiting for it, and its end arrives as a completion.
//
// After a stall every connection has a request waiting, and re-arming every multishot receive that ran out of buffers
// would only run them out again. So such a connection goes on a read queue, read with single receives that never
// outnumber a ring's buffers, and back to its multishot receive once its socket is empty. A pass answers at most
// `answersPerPass` requests; what it reads beyond that waits in the connection's buffer, on a ready list later passes
// answer first, so the kernel's socket buffers empty while passes stay short (rust-iouring-kensac's recovery).
import Glibc
import IORing
import Syscalls

final class Reactor {
    private let signer: TokenSigner
    private let host: String
    private let port: UInt16
    private let settings: Settings
    private var store: Store
    /// The reads walk SQLite's B-trees (else SQL answers them).
    var directReads: Bool { store.directReads }

    /// `SO_REUSEPORT` listeners on the port: each has its own accept queue (capped at `somaxconn`, 4,096 on the box), so
    /// together they hold a ramp's or a stall's new connections instead of refusing them.
    private var listeners: [Int32] = []
    /// The ring each listener's multishot accept is armed on, or nil.
    private var acceptingOn: [Int?] = []
    private var rings = UniqueArray<Ring>()
    /// Connections with responses to send at the end of the pass.
    private var toSend: [ConnRef] = []
    /// Connections whose multishot receive ended, to read with single receives (`readRounds`).
    private var readQueue = Fifo<ConnID>()
    /// Connections with complete requests waiting in their buffer for a pass with answers left.
    private var ready = Fifo<ConnID>()
    private var answersLeft = 0
    /// Send buffers whose sends completed, emptied with their capacity kept: a response is written into one, so in
    /// steady state no response allocates.
    private var sendPool = UniqueArray<ByteBuffer>()
    private var lastPassWasIdle = false

    /// Nil: checkpoints run in the loop (`CHECKPOINT_THREAD=0`).
    private let checkpointer: Checkpointer?
    /// The eventfd the checkpointer writes when it is done, and the 8 bytes the ring reads it into.
    private let checkpointDone: Int32
    private let checkpointSignal = UnsafeMutableRawBufferPointer.allocate(byteCount: 8, alignment: 8)
    private var checkpointPhase = CheckpointPhase.idle
    private var checkpointStarted: UInt64 = 0
    /// While the WAL is due to start over, writes wait (and their connections parse nothing further, so responses
    /// stay in order) and reads go on, until it is all copied: SQLite starts the WAL over only at a write that begins
    /// with every frame copied, which group commit under steady writes never leaves, so the WAL, and the index SQLite
    /// keeps of it in memory, would grow without end.
    private var holdingWrites = false
    private var holdStarted: UInt64 = 0
    private var held: [HeldWrite] = []
    private static let checkpointIntervalNanos: UInt64 = 250_000_000
    private static let holdLimitNanos: UInt64 = 1_000_000_000
    /// Logged once a minute when there were any.
    private var checkpointCounts = CheckpointCounts()
    private var countsLogged: UInt64 = 0

    init(signer: TokenSigner, host: String, port: UInt16, databasePath: String, started: UInt64,
         settings: Settings, checkpointer: Checkpointer?, checkpointDone: Int32) throws(StoreError)
    {
        self.signer = signer
        self.checkpointer = checkpointer
        self.checkpointDone = checkpointDone
        self.host = host
        self.port = port
        self.settings = settings
        store = try Store(path: databasePath, started: started, directReads: settings.directReads)
        toSend.reserveCapacity(1024)
    }

    // ---------- the loop ----------

    func run() -> Never {
        do {
            for _ in 0..<Self.listenerCount { listeners.append(try Sys.bindListener(host: host, port: port)) }
            try addRing()
        } catch {
            fatalError("\(error)")
        }
        acceptingOn = Array(repeating: nil, count: listeners.count)
        if checkpointer != nil { armCheckpointSignal() }
        for l in 0..<listeners.count { armAccept(l) }
        var completions = 1
        while true {
            answersLeft = Self.answersPerPass
            // The other rings: submit and collect without waiting (each has its own table of connections); a ring with
            // nothing to submit and no completions to post needs no enter.
            completions = 0
            for r in 1..<rings.count {
                if rings[r].io.needsEnter { rings[r].io.submit() }
                completions += drain(r)
            }
            // Ring 0: submit and wait for a batch, or the window. With nothing happening, wait for anything; but not
            // with responses waiting for a commit (writes a checkpoint held, answered between passes), and not at all
            // with reads or answers queued.
            let queued = !readQueue.isEmpty || !ready.isEmpty
            let idle = completions == 0 && rings.count == 1 && lastPassWasIdle && toSend.isEmpty && !queued
            if queued {
                rings[0].io.submit()
            } else {
                rings[0].io.submit(waitingFor: idle ? 1 : settings.batch, timeoutNanos: idle ? -1 : settings.windowNanos)
            }
            completions += drain(0)
            // Oldest first: what earlier passes read, then what this one reads.
            runReady()
            completions += readRounds()
            lastPassWasIdle = completions == 0
            endPass()
        }
    }

    /// Takes ring `r`'s completions; returns how many.
    private func drain(_ r: Int) -> Int {
        var n = 0
        while let completion = rings[r].io.nextCompletion() {
            n += 1
            complete(completion, ring: r)
        }
        rings[r].io.finishCompletions()
        return n
    }

    private func complete(_ c: IORing.Completion, ring r: Int) {
        let slot = UInt32(truncatingIfNeeded: c.userData)
        let generation = UInt16(truncatingIfNeeded: c.userData >> 32)
        switch Op(rawValue: c.userData >> 56) {
        case .accept: accepted(c, ring: r, listener: Int(slot))
        case .receive: received(c, ring: r, slot: slot, generation: generation)
        case .receiveOnce: receivedOnce(c, ring: r, slot: slot, generation: generation)
        case .send: sent(c.result, ring: r, slot: slot, generation: generation)
        case .close: closed(ring: r, slot: slot, generation: generation)
        case .checkpointed:
            let copiedAll = checkpointSignal.load(as: UInt64.self) == Checkpointer.copiedAll
            switch checkpointPhase {
            case .copying, .copyingHeld where !holdingWrites:
                // A routine copy is done, or the hold ran out and writes went on.
                checkpointPhase = .idle
            case .copyingHeld where copiedAll:
                // The WAL starts over at the end of the pass (`stepCheckpoint`).
                checkpointPhase = .copiedAll
            case .copyingHeld:
                // Back to back while writes wait.
                startCheckpoint()
            case .restarting:
                checkpointPhase = .restarted
            case .idle, .copiedAll, .restarted:
                break
            }
            armCheckpointSignal()
        case .cancel, .none:
            // A cancel's own completion, or a shutdown's (user data 0).
            break
        }
    }

    /// The pass's transaction commits; then its responses go out.
    private func endPass() {
        // A hold longer than the limit ends (the next pass starts another); not while the checkpointer writes, which
        // a write of the loop's would collide with.
        if holdingWrites && checkpointPhase != .restarting && Sys.monotonicNanos() - holdStarted > Self.holdLimitNanos {
            checkpointCounts.expired += 1
            releaseWrites()
        }
        if !store.finishPass() {
            // Nothing from this pass may claim a write that didn't land: its connections close without an answer.
            Sys.logLine("sqlite: the pass's transaction didn't commit; closing its connections")
            for ref in toSend where rings[ref.ring].conns[ref.index].fromStore { close(ring: ref.ring, slot: ref.slot) }
        }
        for ref in toSend {
            rings[ref.ring].conns[ref.index].queued = false
            rings[ref.ring].conns[ref.index].fromStore = false
            startSend(ring: ref.ring, slot: ref.slot)
        }
        toSend.removeAll(keepingCapacity: true)
        stepCheckpoint()
    }

    /// Checkpoints as mojo-iouring-oh-meh runs them: the checkpointer copies the WAL every 250 ms while writes go on,
    /// and only once the WAL is due to start over do writes wait, while it copies the rest back to back. Then the
    /// checkpointer starts the WAL over itself with a commit that changes nothing, so the sync of the new WAL's header
    /// happens on its thread, not the loop's (c-iouring-shivanshthapliyal's idea). Steps are taken here, between passes
    /// with no transaction open: the copy takes every write committed before the hold, and the restart finds no
    /// reader of the loop's on the old WAL.
    private func stepCheckpoint() {
        guard checkpointer != nil else {
            if store.restartDue { checkpointInLoop() }
            return
        }
        switch checkpointPhase {
        case .idle:
            if store.checkpointOverdue {
                // The checkpointer keeps falling behind: the loop copies the WAL itself, as a last resort.
                checkpointInLoop()
            } else if store.restartDue {
                checkpointCounts.holds += 1
                holdingWrites = true
                holdStarted = Sys.monotonicNanos()
                checkpointPhase = .copyingHeld
                startCheckpoint()
            } else if store.hasWAL && Sys.monotonicNanos() - checkpointStarted >= Self.checkpointIntervalNanos {
                checkpointPhase = .copying
                startCheckpoint()
            }
        case .copiedAll:
            if holdingWrites {
                checkpointPhase = .restarting
                startRestart()
            } else {
                checkpointPhase = .idle
            }
        case .restarted:
            store.checkpointFinished()
            checkpointPhase = .idle
            releaseWrites()
        case .copying, .copyingHeld, .restarting:
            break
        }
        let now = Sys.monotonicNanos()
        if now - countsLogged >= 60_000_000_000 {
            let c = checkpointCounts
            if c.any {
                Sys.logLine("checkpoints in the last minute: \(c.holds) holds, \(c.expired) ran out, \(c.inLoop) in the loop")
            }
            checkpointCounts = CheckpointCounts()
            countsLogged = now
        }
    }

    private func startCheckpoint() {
        guard let checkpointer else { return }
        checkpointStarted = Sys.monotonicNanos()
        // The effect leaves the main thread here: the task starts on the checkpointer's own executor, and the loop
        // goes on.
        Task(executorPreference: checkpointer.executor) { @concurrent in await checkpointer.checkpoint() }
    }

    private func startRestart() {
        guard let checkpointer else { return }
        Task(executorPreference: checkpointer.executor) { @concurrent in await checkpointer.restartWAL() }
    }

    private func checkpointInLoop() {
        checkpointCounts.inLoop += 1
        // The responses go out first: a checkpoint waits for the disk.
        for r in 0..<rings.count { rings[r].io.submit() }
        store.checkpoint()
    }

    /// The writes that waited for the checkpoint, answered in the open transaction (between passes, the next pass's);
    /// then whatever their connections received meanwhile.
    private func releaseWrites() {
        holdingWrites = false
        let writes = held
        held.removeAll(keepingCapacity: true)
        for w in writes {
            let s = Int(w.slot)
            guard rings[w.ring].conns[s].generation == w.generation, rings[w.ring].conns[s].state == .open else { continue }
            rings[w.ring].conns[s].holding = false
            answer(w.query, ring: w.ring, slot: w.slot, close: w.close)
            serveBuffered(ring: w.ring, slot: w.slot)
        }
    }

    private func armCheckpointSignal() {
        rings[0].io.prepare(.read(fd: checkpointDone, buffer: checkpointSignal), userData: Self.userData(.checkpointed, 0, 0, 0))
    }

    // ---------- rings and accepting ----------

    private func addRing() throws(SystemError) {
        // A completion queue larger than a ring's connections, so a burst never spills into the kernel's overflow list.
        let io = try IORing(queueDepth: settings.queueDepth, completionDepth: Self.completionDepth, files: settings.files,
                            bufferCount: settings.bufferCount, bufferSize: settings.bufferSize)
        rings.append(Ring(io: io))
    }

    /// Arms listener `l`'s multishot accept on the first ring with room, adding a ring if none has any.
    private func armAccept(_ l: Int) {
        guard acceptingOn[l] == nil else { return }
        let limit = Int(settings.files) - settings.margin
        var target: Int?
        for r in 0..<rings.count where rings[r].open < limit {
            target = r
            break
        }
        if target == nil && rings.count < settings.maxRings {
            do {
                try addRing()
                target = rings.count - 1
            } catch {
                Sys.logLine("can't add a ring: \(error)")
            }
        }
        guard let r = target else { return }
        acceptingOn[l] = r
        rings[r].acceptCancelled = false
        rings[r].io.prepare(.acceptMultishot(listener: listeners[l]), userData: Self.userData(.accept, r, 0, UInt32(l)))
    }

    private func accepted(_ c: IORing.Completion, ring r: Int, listener l: Int) {
        if c.result >= 0 {
            open(ring: r, slot: UInt32(c.result))
            // Nearly full: this ring stops accepting before its table is, so no connection is taken off a listen
            // queue without a slot; the next ring takes over as the cancels complete.
            if rings[r].open >= Int(settings.files) - settings.margin && !rings[r].acceptCancelled {
                rings[r].acceptCancelled = true
                for other in 0..<listeners.count where acceptingOn[other] == r {
                    let target = Self.userData(.accept, r, 0, UInt32(other))
                    rings[r].io.prepare(.cancel(userData: target), userData: Self.userData(.cancel, r, 0, 0))
                }
            }
        } else if c.result != -ECANCELED {
            Sys.logLine("accept: \(String(cString: strerror(-c.result)))")
        }
        if !c.hasMore {
            acceptingOn[l] = nil
            armAccept(l)
        }
    }

    // ---------- connections ----------

    private func open(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        while rings[r].conns.count <= s { rings[r].conns.append(Conn()) }
        rings[r].conns[s].generation &+= 1
        rings[r].conns[s].state = .open
        rings[r].conns[s].closeAfterSend = false
        rings[r].conns[s].continueSent = false
        rings[r].conns[s].holding = false
        rings[r].conns[s].peerClosed = false
        rings[r].conns[s].readQueued = false
        rings[r].conns[s].reading = false
        rings[r].conns[s].ready = false
        rings[r].open += 1
        armReceive(ring: r, slot: slot)
    }

    private func armReceive(ring r: Int, slot: UInt32) {
        rings[r].conns[Int(slot)].receiving = true
        let generation = rings[r].conns[Int(slot)].generation
        rings[r].io.prepare(.receiveMultishot(slot: slot), userData: Self.userData(.receive, r, generation, slot))
    }

    private func received(_ c: IORing.Completion, ring r: Int, slot: UInt32, generation: UInt16) {
        let s = Int(slot)
        // A provided buffer goes back to the kernel whatever its connection's state.
        guard let buffer = c.buffer else {
            if s < rings[r].conns.count && rings[r].conns[s].generation == generation { receiveEnded(c, ring: r, slot: slot) }
            return
        }
        defer { rings[r].io.recycle(buffer) }
        guard s < rings[r].conns.count, rings[r].conns[s].generation == generation, rings[r].conns[s].state == .open else { return }
        if c.result > 0 {
            consume(rings[r].io.buffer(buffer, count: Int(c.result)), ring: r, slot: slot)
        }
        receiveEnded(c, ring: r, slot: slot)
    }

    /// After a multishot receive's completion: a connection that failed closes; one whose peer is done sending (0,
    /// perhaps only half-closed) gets the answers to what it sent, then closes; a receive that ended (out of buffers,
    /// or data without more to come) is read with single receives from here on.
    private func receiveEnded(_ c: IORing.Completion, ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard rings[r].conns[s].state == .open else { return }
        if c.hasMore { return }
        rings[r].conns[s].receiving = false
        if c.result == 0 {
            peerFinished(ring: r, slot: slot)
        } else if c.result < 0 && c.result != -ENOBUFS {
            close(ring: r, slot: slot)
        } else {
            queueRead(ring: r, slot: slot)
        }
    }

    private func peerFinished(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        rings[r].conns[s].peerClosed = true
        if rings[r].conns[s].out.isEmpty && rings[r].conns[s].sending.isEmpty && !rings[r].conns[s].holding
            && !rings[r].conns[s].ready
        {
            close(ring: r, slot: slot)
        }
    }

    // ---------- reading after a burst ----------

    private func queueRead(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard !rings[r].conns[s].readQueued else { return }
        rings[r].conns[s].readQueued = true
        readQueue.append(ConnID(ring: r, slot: slot, generation: rings[r].conns[s].generation))
    }

    /// The queued connections, read in rounds: a round issues one single receive per connection, never more on a ring
    /// than it has buffers, and each ring's receives complete inside its one non-blocking enter, so a request is
    /// answered in the pass that read it. Returns the completions taken.
    private func readRounds() -> Int {
        var completions = 0
        var budget = Self.readsPerPass
        while budget > 0 && !readQueue.isEmpty {
            let issued = issueReads(budget)
            if issued == 0 { break }
            budget -= issued
            for r in 0..<rings.count where rings[r].io.needsEnter { rings[r].io.submit() }
            for r in 0..<rings.count { completions += drain(r) }
        }
        return completions
    }

    /// Issues up to `budget` single receives for the read queue, in order; returns how many.
    private func issueReads(_ budget: Int) -> Int {
        var issued = 0
        for _ in 0..<readQueue.count {
            guard issued < budget, let id = readQueue.popFirst() else { break }
            let s = Int(id.slot)
            guard rings[id.ring].conns[s].generation == id.generation, rings[id.ring].conns[s].state == .open else { continue }
            if rings[id.ring].conns[s].receiving {
                // Back on its multishot receive already.
                rings[id.ring].conns[s].readQueued = false
                continue
            }
            if rings[id.ring].conns[s].reading || rings[id.ring].readsInFlight >= rings[id.ring].io.bufferCount {
                readQueue.append(id)
                continue
            }
            rings[id.ring].conns[s].readQueued = false
            rings[id.ring].conns[s].reading = true
            rings[id.ring].readsInFlight += 1
            rings[id.ring].io.prepare(.receive(slot: id.slot), userData: Self.userData(.receiveOnce, id.ring, id.generation, id.slot))
            issued += 1
        }
        return issued
    }

    /// A single receive's completion: a full buffer may have more behind it, so the connection is read again; a
    /// shorter one, or an empty socket, sends it back to its multishot receive.
    private func receivedOnce(_ c: IORing.Completion, ring r: Int, slot: UInt32, generation: UInt16) {
        let s = Int(slot)
        rings[r].readsInFlight -= 1
        let buffer = c.buffer
        defer { if let buffer { rings[r].io.recycle(buffer) } }
        guard s < rings[r].conns.count, rings[r].conns[s].generation == generation, rings[r].conns[s].state == .open else { return }
        rings[r].conns[s].reading = false
        if c.result > 0, let buffer {
            consume(rings[r].io.buffer(buffer, count: Int(c.result)), ring: r, slot: slot)
            guard rings[r].conns[s].state == .open else { return }
            if Int(c.result) == rings[r].io.bufferSize {
                queueRead(ring: r, slot: slot)
            } else {
                armReceive(ring: r, slot: slot)
            }
            return
        }
        switch c.result {
        case 0: peerFinished(ring: r, slot: slot)
        case -EAGAIN: armReceive(ring: r, slot: slot)
        case -ENOBUFS: queueRead(ring: r, slot: slot)
        default: close(ring: r, slot: slot)
        }
    }

    /// Connections whose requests waited for answers left, oldest first, while this pass has some.
    private func runReady() {
        while answersLeft > 0, let id = ready.popFirst() {
            let s = Int(id.slot)
            guard rings[id.ring].conns[s].generation == id.generation, rings[id.ring].conns[s].state == .open else { continue }
            rings[id.ring].conns[s].ready = false
            serveBuffered(ring: id.ring, slot: id.slot)
            if rings[id.ring].conns[s].peerClosed && rings[id.ring].conns[s].state == .open { peerFinished(ring: id.ring, slot: id.slot) }
        }
    }

    private func markReady(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard !rings[r].conns[s].ready else { return }
        rings[r].conns[s].ready = true
        ready.append(ConnID(ring: r, slot: slot, generation: rings[r].conns[s].generation))
    }

    /// Received bytes: parsed in place when nothing was left over from before, else after what was.
    private func consume(_ bytes: UnsafeBufferPointer<UInt8>, ring r: Int, slot: UInt32) {
        let s = Int(slot)
        if rings[r].conns[s].rx.isEmpty {
            let span = Span(_unsafeElements: bytes)
            let used = serve(span, ring: r, slot: slot)
            if used < bytes.count && rings[r].conns[s].state == .open && !rings[r].conns[s].closeAfterSend {
                rings[r].conns[s].rx.append(span.extracting(used..<bytes.count))
            }
            return
        }
        // The leftover comes out of the slot while it is parsed, as the requests in it write to the slot.
        var pending = ByteBuffer()
        swap(&pending, &rings[r].conns[s].rx)
        pending.append(UnsafeRawBufferPointer(bytes))
        let used = serve(pending.span, ring: r, slot: slot)
        if used < pending.count && rings[r].conns[s].state == .open && !rings[r].conns[s].closeAfterSend {
            pending.removeFirst(used)
            swap(&pending, &rings[r].conns[s].rx)
        }
    }

    /// Answers the complete requests at the start of `buf`; returns how many bytes they took.
    private func serve(_ buf: Span<UInt8>, ring r: Int, slot: UInt32) -> Int {
        let s = Int(slot)
        var at = 0
        while at < buf.count {
            if rings[r].conns[s].state != .open || rings[r].conns[s].closeAfterSend { return buf.count }
            // A held write: what follows it waits in the connection's buffer.
            if rings[r].conns[s].holding { return at }
            // No answers left in this pass: the rest waits in the buffer for a later one.
            if answersLeft <= 0 {
                markReady(ring: r, slot: slot)
                return at
            }
            let outcome = HTTP.parseRequest(signer, buf.extracting(at..<buf.count))
            switch consume outcome {
            case .complete(let consumed, let closeAfter, let dispatch):
                rings[r].conns[s].continueSent = false
                answersLeft -= 1
                respond(dispatch, ring: r, slot: slot, close: closeAfter)
                at += consumed
            case .incomplete(let sendContinue):
                if sendContinue && !rings[r].conns[s].continueSent {
                    rings[r].conns[s].continueSent = true
                    queueBytes(UnsafeRawBufferPointer(start: Self.continueHead.utf8Start, count: Self.continueHead.utf8CodeUnitCount), ring: r, slot: slot)
                }
                return at
            case .reject(let status):
                answerError(status, HTTP.errorMessage(status), ring: r, slot: slot, close: true)
                return buf.count
            }
        }
        return at
    }

    private func respond(_ dispatch: consuming Router.Dispatch, ring r: Int, slot: UInt32, close: Bool) {
        switch consume dispatch {
        case .error(let status, let message):
            answerError(status, message, ring: r, slot: slot, close: close)
        case .read(let query):
            answer(query, ring: r, slot: slot, close: close)
        case .write(let query):
            if holdingWrites {
                rings[r].conns[Int(slot)].holding = true
                held.append(HeldWrite(ring: r, slot: slot, generation: rings[r].conns[Int(slot)].generation, query: query, close: close))
                return
            }
            answer(query, ring: r, slot: slot, close: close)
        }
    }

    /// Requests that waited in the connection's buffer (behind a held write).
    private func serveBuffered(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard !rings[r].conns[s].rx.isEmpty, rings[r].conns[s].state == .open, !rings[r].conns[s].closeAfterSend else { return }
        var pending = ByteBuffer()
        swap(&pending, &rings[r].conns[s].rx)
        let used = serve(pending.span, ring: r, slot: slot)
        if used < pending.count && rings[r].conns[s].state == .open && !rings[r].conns[s].closeAfterSend {
            pending.removeFirst(used)
            swap(&pending, &rings[r].conns[s].rx)
        }
    }

    /// Reads and writes alike run in the pass's transaction.
    private func answer(_ query: Query, ring r: Int, slot: UInt32, close: Bool) {
        let start = beginResponse(ring: r, slot: slot)
        let status = store.answer(query, into: &rings[r].conns[Int(slot)].out)
        rings[r].conns[Int(slot)].fromStore = true
        finishResponse(status, start: start, ring: r, slot: slot, close: close)
    }

    // ---------- responses, written in place ----------

    /// SPEC.md's `{"error":"<message>"}`.
    private func answerError(_ status: UInt16, _ message: StaticString, ring r: Int, slot: UInt32, close: Bool) {
        let start = beginResponse(ring: r, slot: slot)
        JSON.writeError(&rings[r].conns[Int(slot)].out, message)
        finishResponse(status, start: start, ring: r, slot: slot, close: close)
    }

    /// Starts a response at the end of the connection's output (a pooled buffer if it has none): room for the head,
    /// which `finishResponse` writes once the body's length is known. Returns where the response starts.
    private func beginResponse(ring r: Int, slot: UInt32) -> Int {
        let s = Int(slot)
        provideBuffer(ring: r, slot: slot)
        let start = rings[r].conns[s].out.count
        rings[r].conns[s].out.append(repeating: 0, count: Self.headRoom)
        return start
    }

    /// Writes the head right in front of the body after `start`, so the response is contiguous. The first response in
    /// the buffer leaves the unused room in front of it (its send starts after it); one behind another (pipelined)
    /// moves down over it.
    private func finishResponse(_ status: UInt16, start: Int, ring r: Int, slot: UInt32, close: Bool) {
        let s = Int(slot)
        let length = rings[r].conns[s].out.count - start - Self.headRoom
        var head = InlineArray<160, UInt8>(repeating: 0)
        var headSpan = head.mutableSpan
        let n = HTTP.formatHeader(into: &headSpan, status, length, close)
        let gap = Self.headRoom - n
        rings[r].conns[s].out.store(head.span.extracting(0..<n), at: start + gap)
        if start == 0 {
            rings[r].conns[s].outFrom = gap
        } else {
            rings[r].conns[s].out.moveDown(from: start + gap, to: start, count: n + length)
            rings[r].conns[s].out.truncate(start + n + length)
        }
        if close { rings[r].conns[s].closeAfterSend = true }
        markToSend(ring: r, slot: slot)
    }

    private func queueBytes(_ bytes: UnsafeRawBufferPointer, ring r: Int, slot: UInt32) {
        provideBuffer(ring: r, slot: slot)
        rings[r].conns[Int(slot)].out.append(bytes)
        markToSend(ring: r, slot: slot)
    }

    private func provideBuffer(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard rings[r].conns[s].out.capacity == 0 else { return }
        if let pooled = sendPool.popLast() {
            rings[r].conns[s].out = pooled
        } else {
            rings[r].conns[s].out = ByteBuffer(capacity: Self.sendBufferSize)
        }
    }

    /// A buffer back to the pool, its capacity kept: unless it has none, grew past `keepCapacity`, or the pool is full.
    private func recycle(_ buffer: consuming ByteBuffer) {
        guard buffer.capacity > 0, buffer.capacity <= Self.keepCapacity, sendPool.count < Self.poolLimit else { return }
        buffer.removeAll()
        sendPool.append(buffer)
    }

    private func markToSend(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        if !rings[r].conns[s].queued {
            rings[r].conns[s].queued = true
            toSend.append(ConnRef(ring: r, slot: slot))
        }
    }

    /// Hands the connection's output to a send, unless one is in flight (it follows that one).
    private func startSend(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard rings[r].conns[s].state == .open, rings[r].conns[s].sending.isEmpty, !rings[r].conns[s].out.isEmpty else { return }
        var bytes = ByteBuffer()
        swap(&bytes, &rings[r].conns[s].out)
        swap(&bytes, &rings[r].conns[s].sending)
        rings[r].conns[s].sentCount = rings[r].conns[s].outFrom
        rings[r].conns[s].outFrom = 0
        sendRest(ring: r, slot: slot)
    }

    /// Sends what of `sending` hasn't gone yet. Its bytes stay put until the send completes: nothing touches `sending`
    /// meanwhile, so its storage doesn't move.
    private func sendRest(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        let from = rings[r].conns[s].sentCount
        let bytes = rings[r].conns[s].sending.withBytes { UnsafeRawBufferPointer(rebasing: $0[from...]) }
        let generation = rings[r].conns[s].generation
        rings[r].io.prepare(.send(slot: slot, bytes: bytes), userData: Self.userData(.send, r, generation, slot))
    }

    private func sent(_ result: Int32, ring r: Int, slot: UInt32, generation: UInt16) {
        let s = Int(slot)
        guard s < rings[r].conns.count, rings[r].conns[s].generation == generation, rings[r].conns[s].state != .free else { return }
        if result >= 0 && rings[r].conns[s].state == .open {
            rings[r].conns[s].sentCount += Int(result)
            if rings[r].conns[s].sentCount < rings[r].conns[s].sending.count {
                sendRest(ring: r, slot: slot)
                return
            }
        }
        var done = ByteBuffer()
        swap(&done, &rings[r].conns[s].sending)
        recycle(done)
        rings[r].conns[s].sentCount = 0
        switch rings[r].conns[s].state {
        case .closing:
            // The close waited for this send (a shutdown already went out).
            rings[r].io.prepare(.close(slot: slot), userData: Self.userData(.close, r, generation, slot))
        case .open:
            if result < 0 {
                close(ring: r, slot: slot)
            } else if !rings[r].conns[s].out.isEmpty {
                startSend(ring: r, slot: slot)
            } else if rings[r].conns[s].closeAfterSend
                || (rings[r].conns[s].peerClosed && !rings[r].conns[s].holding && !rings[r].conns[s].ready)
            {
                close(ring: r, slot: slot)
            }
        case .free:
            break
        }
    }

    /// Closes a connection: a shutdown ends its receive, then the close frees its slot. With a send in flight the close
    /// waits for the send's completion.
    private func close(ring r: Int, slot: UInt32) {
        let s = Int(slot)
        guard rings[r].conns[s].state == .open else { return }
        rings[r].conns[s].state = .closing
        rings[r].conns[s].rx.release()
        var unsent = ByteBuffer()
        swap(&unsent, &rings[r].conns[s].out)
        recycle(unsent)
        rings[r].conns[s].outFrom = 0
        let generation = rings[r].conns[s].generation
        if !rings[r].conns[s].sending.isEmpty {
            rings[r].io.prepare(.shutdown(slot: slot), userData: 0)
        } else if rings[r].conns[s].receiving || rings[r].conns[s].reading {
            rings[r].io.prepare(.shutdownAndClose(slot: slot), userData: Self.userData(.close, r, generation, slot))
        } else {
            rings[r].io.prepare(.close(slot: slot), userData: Self.userData(.close, r, generation, slot))
        }
    }

    private func closed(ring r: Int, slot: UInt32, generation: UInt16) {
        let s = Int(slot)
        guard s < rings[r].conns.count, rings[r].conns[s].generation == generation, rings[r].conns[s].state == .closing else { return }
        rings[r].conns[s].state = .free
        rings[r].conns[s].receiving = false
        rings[r].conns[s].reading = false
        rings[r].conns[s].sending.release()
        rings[r].open -= 1
        // A listener whose accept found no ring with room accepts again once one has some.
        if rings[r].open < Int(settings.files) - 2 * settings.margin {
            for l in 0..<listeners.count where acceptingOn[l] == nil { armAccept(l) }
        }
    }
}

extension Reactor {
    struct Settings {
        /// Direct descriptors per ring: one ring's connections.
        let files: UInt32
        /// A ring stops accepting this many slots before its table is full.
        let margin: Int
        let maxRings: Int
        let queueDepth: UInt32
        let bufferCount: UInt32
        let bufferSize: UInt32
        /// Under load a pass waits for `batch` completions or `windowNanos`, whichever comes first.
        let batch: UInt32
        let windowNanos: Int64
        /// Reads walk SQLite's B-trees (`BTREE_READS=0`: through SQL, to compare).
        let directReads: Bool
    }

    /// One ring and the connections in its table, indexed by slot.
    struct Ring: ~Copyable {
        var io: IORing
        var conns = UniqueArray<Conn>(capacity: 1024)
        var open = 0
        /// Its accepts are being cancelled: it is nearly full.
        var acceptCancelled = false
        /// Single receives submitted and not completed, each holding (or about to hold) one of its buffers.
        var readsInFlight = 0

        init(io: consuming IORing) {
            self.io = io
        }
    }

    /// What the checkpointer is doing.
    enum CheckpointPhase {
        case idle
        /// Copying the WAL while writes go on.
        case copying
        /// Copying the WAL while writes wait for it to start over.
        case copyingHeld
        /// All of it is copied: at the end of the pass the checkpointer starts the WAL over.
        case copiedAll
        /// The checkpointer's commit that starts the WAL over runs; writes still wait.
        case restarting
        /// It ran: writes go on at the end of the pass.
        case restarted
    }

    /// Holds of writes, those that ran out, and the loop's own copies.
    struct CheckpointCounts {
        var holds = 0
        var expired = 0
        var inLoop = 0
        var any: Bool { holds + expired + inLoop > 0 }
    }

    enum ConnState {
        case free
        case open
        /// Shut down; the slot is freed when its close completes.
        case closing
    }

    /// A connection: its buffers (owned, so the slot is noncopyable) and its state. `generation` tells its completions
    /// from those of an earlier connection in the same slot.
    struct Conn: ~Copyable {
        var generation: UInt16 = 0
        var state = ConnState.free
        var receiving = false
        /// What arrived past the last complete request.
        var rx = ByteBuffer()
        /// Responses waiting for the end of the pass, or for the send in flight.
        var out = ByteBuffer()
        /// Where the first response in `out` starts (the head's unused room is in front of it).
        var outFrom = 0
        /// The bytes of the send in flight.
        var sending = ByteBuffer()
        var sentCount = 0
        var closeAfterSend = false
        var continueSent = false
        /// Its write waits for a checkpoint (`held`).
        var holding = false
        /// The peer is done sending: once what it sent is answered, the connection closes.
        var peerClosed = false
        /// In `toSend`.
        var queued = false
        /// In `readQueue`.
        var readQueued = false
        /// A single receive is in flight.
        var reading = false
        /// In `ready`: complete requests wait in `rx`.
        var ready = false
        /// Has a response from this pass's transaction.
        var fromStore = false
    }

    struct HeldWrite {
        let ring: Int
        let slot: UInt32
        let generation: UInt16
        let query: Query
        let close: Bool
    }

    struct ConnRef {
        let ring: Int
        let slot: UInt32
        var index: Int { Int(slot) }
    }

    /// A connection in a queue that outlives the pass: its generation tells it from a later one in the same slot.
    struct ConnID {
        let ring: Int
        let slot: UInt32
        let generation: UInt16
    }

    /// What a completion is for: the top byte of its user data. The rest is the ring, the connection's generation and
    /// its slot.
    enum Op: UInt64 {
        case accept = 1
        case receive
        case receiveOnce
        case send
        case close
        case cancel
        case checkpointed
    }

    @inline(__always) static func userData(_ op: Op, _ ring: Int, _ generation: UInt16, _ slot: UInt32) -> UInt64 {
        op.rawValue << 56 | UInt64(ring) << 48 | UInt64(generation) << 32 | UInt64(slot)
    }

    private static let continueHead: StaticString = "HTTP/1.1 100 Continue\r\n\r\n"
    /// Room for a response's head: the longest status line, Content-Type, Content-Length and Connection fit.
    private static let headRoom = 160
    private static let sendBufferSize = 8 * 1024
    private static let keepCapacity = 64 * 1024
    private static let poolLimit = 4096
    private static let listenerCount = 8
    /// At most this many single receives in a pass, and this many requests answered.
    private static let readsPerPass = 16_384
    private static let answersPerPass = 2_048
    private static let completionDepth: UInt32 = 65_536
}
