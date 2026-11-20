// io_uring without liburing or C: the raw system calls (io_uring_setup, io_uring_enter, io_uring_register), the rings
// mapped with mmap, entries and completions written and read at the kernel's offsets (linux/io_uring.h, the same on
// x86-64 and arm64), and the heads and tails moved with the acquire and release orderings the ring protocol asks for.
// The shape follows swift-system's IORing (prepare requests, submit, consume completions), with the socket requests
// that one doesn't offer yet: multishot accept into direct descriptors, multishot and single receives into provided
// buffers, send, shutdown and close.
import Glibc
public import Synchronization
public import Syscalls

/// An io_uring instance with a table of direct descriptors and a ring of provided receive buffers. Noncopyable: it
/// owns its mappings, and only the thread that created it may submit (IORING_SETUP_SINGLE_ISSUER).
public struct IORing: ~Copyable, ~Sendable {
    @usableFromInline let fd: Int32
    // Submission queue: the kernel's head, tail and flags, and the entries. `sqeTail` runs ahead of the kernel's tail
    // until `submit` publishes it.
    @usableFromInline let sqHead: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqTail: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqFlags: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqMask: UInt32
    @usableFromInline let sqEntries: UInt32
    @usableFromInline var sqeTail: UInt32
    @usableFromInline let sqes: UnsafeMutableRawPointer
    // Completion queue: `cqHead` is ours, published by `finishCompletions`.
    @usableFromInline let cqHeadShared: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let cqTail: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let cqMask: UInt32
    @usableFromInline var cqHead: UInt32
    @usableFromInline let cqes: UnsafeRawPointer
    private let maps: Maps
    /// io_uring_enter's wait argument: an io_uring_getevents_arg pointing at the __kernel_timespec after it.
    @usableFromInline let waitArgument: UnsafeMutableRawPointer
    // The provided buffers: their ring (io_uring_buf entries, whose shared tail sits in the first entry's last two
    // bytes), and the buffers themselves, `bufferCount` of `bufferSize` bytes.
    @usableFromInline let bufferRing: UnsafeMutableRawPointer
    @usableFromInline let bufferRingTail: UnsafeMutablePointer<Atomic<UInt16>>
    @usableFromInline let bufferBase: UnsafeMutableRawPointer
    @usableFromInline var bufferTail: UInt16 = 0
    public let bufferCount: Int
    public let bufferSize: Int

    /// What the server asks of a ring.
    public enum Request {
        /// Accepts connections on `listener` until cancelled, each into a free direct descriptor.
        case acceptMultishot(listener: Int32)
        /// Receives on direct descriptor `slot` until it fails, each chunk into a provided buffer.
        case receiveMultishot(slot: UInt32)
        /// Receives once on direct descriptor `slot` into a provided buffer, without waiting: what is there completes
        /// inside the submit, an empty socket with -EAGAIN.
        case receive(slot: UInt32)
        /// Sends all of `bytes` (which must stay put until the completion).
        case send(slot: UInt32, bytes: UnsafeRawBufferPointer)
        /// Shuts the socket down both ways, which ends its multishot receive, and closes it after that. Only the close
        /// completes with the user data; the shutdown completes with 0.
        case shutdownAndClose(slot: UInt32)
        /// Shuts the socket down both ways (when a send is still in flight: the close comes after it). Completes with
        /// user data 0.
        case shutdown(slot: UInt32)
        case close(slot: UInt32)
        /// Reads a plain descriptor (an eventfd) into `buffer`, which must stay put until the completion.
        case read(fd: Int32, buffer: UnsafeMutableRawBufferPointer)
        /// Cancels the request submitted with `userData`.
        case cancel(userData: UInt64)
    }

    public struct Completion {
        public let userData: UInt64
        /// The request's result: a count, a descriptor slot, or -errno.
        public let result: Int32
        public let flags: UInt32

        @inlinable init(userData: UInt64, result: Int32, flags: UInt32) {
            self.userData = userData
            self.result = result
            self.flags = flags
        }

        /// A multishot request is still armed.
        @inlinable public var hasMore: Bool { flags & ABI.cqeMore != 0 }

        /// The provided buffer a receive filled, if any.
        @inlinable public var buffer: UInt16? {
            flags & ABI.cqeBuffer != 0 ? UInt16(truncatingIfNeeded: flags >> ABI.cqeBufferShift) : nil
        }
    }

    /// A ring of `queueDepth` submissions and `completionDepth` completions, `files` direct descriptors and `bufferCount`
    /// receive buffers of `bufferSize` bytes (a power of two of them). Only this thread submits to it (SINGLE_ISSUER),
    /// and it posts completions when this thread asks for them (DEFER_TASKRUN), so they come in batches, without
    /// interrupts; the kernel flags when there are some to post (TASKRUN_FLAG), so a ring with nothing to do needs no
    /// enter.
    public init(queueDepth: UInt32, completionDepth: UInt32, files: UInt32, bufferCount: UInt32, bufferSize: UInt32) throws(SystemError) {
        let flags = ABI.setupSingleIssuer | ABI.setupDeferTaskrun | ABI.setupSubmitAll | ABI.setupTaskrunFlag
        // Without the submission index array where the kernel can (6.6 on); with it, each place maps to its entry.
        let rings: Rings
        do {
            rings = try Rings(queueDepth, completionDepth, flags | ABI.setupNoSQArray)
        } catch where error.code == EINVAL {
            rings = try Rings(queueDepth, completionDepth, flags)
        }
        let buffers: Buffers
        do {
            try Self.registerFiles(rings.fd, count: files)
            buffers = try Buffers(ringFD: rings.fd, count: bufferCount, size: bufferSize)
        } catch {
            rings.unmap()
            throw error
        }
        fd = rings.fd
        sqHead = rings.sqHead
        sqTail = rings.sqTail
        sqFlags = rings.sqFlags
        sqMask = rings.sqMask
        sqEntries = rings.sqEntries
        sqeTail = rings.sqTail.pointee.load(ordering: .relaxed)
        sqes = rings.sqes
        cqHeadShared = rings.cqHead
        cqTail = rings.cqTail
        cqMask = rings.cqMask
        cqHead = rings.cqHead.pointee.load(ordering: .relaxed)
        cqes = UnsafeRawPointer(rings.cqes)
        maps = rings.maps
        waitArgument = UnsafeMutableRawPointer.allocate(byteCount: ABI.getEventsArgSize + 16, alignment: 8)
        waitArgument.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.getEventsArgSize + 16)
        waitArgument.storeBytes(of: UInt64(UInt(bitPattern: waitArgument + ABI.getEventsArgSize)),
                                toByteOffset: ABI.getEventsArgTimespec, as: UInt64.self)
        bufferRing = buffers.ring
        bufferRingTail = (buffers.ring + ABI.bufferRingTail).assumingMemoryBound(to: Atomic<UInt16>.self)
        bufferBase = buffers.base
        self.bufferCount = Int(bufferCount)
        self.bufferSize = Int(bufferSize)
        for id in 0..<bufferCount { recycle(UInt16(id)) }
        bufferRingTail.pointee.store(bufferTail, ordering: .releasing)
    }

    deinit {
        // The buffer ring's registration (group 0) goes first: the kernel may still hold it.
        let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.bufferRegSize, alignment: 8)
        registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.bufferRegSize)
        _ = sys_syscall(ABI.sysRegister, Int(fd), ABI.registerUnregisterBufferRing, Int(bitPattern: registration), 1, 0, 0)
        registration.deallocate()
        munmap(bufferRing, bufferCount * ABI.bufferEntrySize)
        free(bufferBase)
        waitArgument.deallocate()
        maps.unmap()
        Glibc.close(fd)
    }

    // ---------- submitting ----------

    /// The next free submission entry, zeroed; when the queue is full, what is in it is submitted first.
    @inlinable @inline(__always) mutating func entry() -> UnsafeMutableRawPointer {
        if sqeTail &- sqHead.pointee.load(ordering: .acquiring) >= sqEntries {
            submit()
            guard sqeTail &- sqHead.pointee.load(ordering: .acquiring) < sqEntries else {
                fatalError("io_uring submission queue stuck")
            }
        }
        let sqe = sqes + Int(sqeTail & sqMask) * ABI.sqeSize
        sqeTail &+= 1
        sqe.initializeMemory(as: UInt64.self, repeating: 0, count: ABI.sqeSize / 8)
        return sqe
    }

    /// Free submission entries.
    @inlinable @inline(__always) var space: UInt32 {
        sqEntries &- (sqeTail &- sqHead.pointee.load(ordering: .acquiring))
    }

    /// Prepares `request`; it goes to the kernel with the next `submit`.
    @inlinable @inline(__always) public mutating func prepare(_ request: Request, userData: UInt64) {
        switch request {
        case .acceptMultishot(let listener):
            let sqe = entry()
            sqe.opcode = ABI.opAccept
            sqe.fd = listener
            sqe.ioprio = ABI.acceptMultishot
            sqe.fileIndex = ABI.fileIndexAlloc
            sqe.userData = userData
        case .receiveMultishot(let slot):
            let sqe = entry()
            sqe.opcode = ABI.opReceive
            sqe.fd = Int32(bitPattern: slot)
            sqe.sqeFlags = ABI.sqeFixedFile | ABI.sqeBufferSelect
            sqe.ioprio = ABI.receiveMultishot
            sqe.bufferGroup = ABI.bufferGroup
            sqe.userData = userData
        case .receive(let slot):
            let sqe = entry()
            sqe.opcode = ABI.opReceive
            sqe.fd = Int32(bitPattern: slot)
            sqe.sqeFlags = ABI.sqeFixedFile | ABI.sqeBufferSelect
            sqe.bufferGroup = ABI.bufferGroup
            sqe.opFlags = ABI.messageDontWait
            sqe.userData = userData
        case .send(let slot, let bytes):
            let sqe = entry()
            sqe.opcode = ABI.opSend
            sqe.fd = Int32(bitPattern: slot)
            sqe.sqeFlags = ABI.sqeFixedFile
            sqe.address = UInt64(UInt(bitPattern: bytes.baseAddress))
            sqe.length = UInt32(bytes.count)
            sqe.opFlags = ABI.messageNoSignal | ABI.messageWaitAll
            sqe.userData = userData
        case .shutdownAndClose(let slot):
            // Both entries in one go, so the link can't be split by a submission in between.
            if space < 2 { submit() }
            prepareShutdown(slot, linked: true)
            prepareClose(slot, userData: userData)
        case .shutdown(let slot):
            prepareShutdown(slot, linked: false)
        case .close(let slot):
            prepareClose(slot, userData: userData)
        case .read(let fd, let buffer):
            let sqe = entry()
            sqe.opcode = ABI.opRead
            sqe.fd = fd
            sqe.address = UInt64(UInt(bitPattern: buffer.baseAddress))
            sqe.length = UInt32(buffer.count)
            sqe.offset = UInt64.max // the descriptor's current position
            sqe.userData = userData
        case .cancel(let target):
            let sqe = entry()
            sqe.opcode = ABI.opAsyncCancel
            sqe.fd = -1
            sqe.address = target
            sqe.userData = userData
        }
    }

    /// A shutdown of `slot` both ways (its completion carries user data 0); `linked`: the next entry runs after it.
    @inlinable @inline(__always) mutating func prepareShutdown(_ slot: UInt32, linked: Bool) {
        let sqe = entry()
        sqe.opcode = ABI.opShutdown
        sqe.fd = Int32(bitPattern: slot)
        sqe.sqeFlags = ABI.sqeFixedFile | (linked ? ABI.sqeHardLink : 0)
        sqe.length = ABI.shutdownBoth
    }

    /// Closes direct descriptor `slot`, freeing it for the next accept.
    @inlinable @inline(__always) mutating func prepareClose(_ slot: UInt32, userData: UInt64) {
        let sqe = entry()
        sqe.opcode = ABI.opClose
        sqe.fileIndex = slot &+ 1
        sqe.userData = userData
    }

    /// Submits what was prepared, then waits for `count` completions or `timeoutNanos` (negative: no timeout).
    /// Always asks for completions (IORING_ENTER_GETEVENTS), which a DEFER_TASKRUN ring posts only then. Returns the
    /// number submitted, or -errno (-ETIME when the wait timed out).
    @inlinable @inline(__always) @discardableResult
    public mutating func submit(waitingFor count: UInt32 = 0, timeoutNanos: Int64 = -1) -> Int32 {
        let submitted = sqeTail &- sqTail.pointee.load(ordering: .relaxed)
        if submitted != 0 { sqTail.pointee.store(sqeTail, ordering: .releasing) }
        var flags = ABI.enterGetEvents
        var argument = 0
        var argumentSize = 0
        if count > 0 && timeoutNanos >= 0 {
            waitArgument.storeBytes(of: timeoutNanos / 1_000_000_000, toByteOffset: ABI.getEventsArgSize, as: Int64.self)
            waitArgument.storeBytes(of: timeoutNanos % 1_000_000_000, toByteOffset: ABI.getEventsArgSize + 8, as: Int64.self)
            flags |= ABI.enterExtArg
            argument = Int(bitPattern: waitArgument)
            argumentSize = ABI.getEventsArgSize
        }
        let rc = sys_syscall(ABI.sysEnter, Int(fd), Int(submitted), Int(count), Int(flags), argument, argumentSize)
        return rc < 0 ? -errno : Int32(truncatingIfNeeded: rc)
    }

    /// Whether a `submit` would do anything: entries to submit, completions waiting for this thread to post them
    /// (IORING_SQ_TASKRUN), or completions that overflowed the queue.
    @inlinable @inline(__always) public var needsEnter: Bool {
        let flags = sqFlags.pointee.load(ordering: .acquiring)
        return sqeTail != sqTail.pointee.load(ordering: .relaxed) || flags & (ABI.sqTaskrun | ABI.sqCQOverflow) != 0
    }

    // ---------- completions ----------

    /// The next completion, consumed; the kernel sees the space at `finishCompletions()`.
    @inlinable @inline(__always) public mutating func nextCompletion() -> Completion? {
        guard cqHead != cqTail.pointee.load(ordering: .acquiring) else { return nil }
        let cqe = cqes + Int(cqHead & cqMask) * ABI.cqeSize
        let completion = Completion(userData: cqe.load(fromByteOffset: ABI.cqeUserData, as: UInt64.self),
                                    result: cqe.load(fromByteOffset: ABI.cqeResult, as: Int32.self),
                                    flags: cqe.load(fromByteOffset: ABI.cqeFlags, as: UInt32.self))
        cqHead &+= 1
        return completion
    }

    /// Hands the consumed completions' space and the recycled buffers back to the kernel.
    @inlinable @inline(__always) public mutating func finishCompletions() {
        cqHeadShared.pointee.store(cqHead, ordering: .releasing)
        bufferRingTail.pointee.store(bufferTail, ordering: .releasing)
    }

    // ---------- provided buffers ----------

    /// The first `count` bytes of provided buffer `id`: valid until the buffer is recycled.
    @inlinable @inline(__always) public func buffer(_ id: UInt16, count: Int) -> UnsafeBufferPointer<UInt8> {
        let base = UnsafeRawPointer(bufferBase + Int(id) * bufferSize).assumingMemoryBound(to: UInt8.self)
        return UnsafeBufferPointer(start: base, count: count)
    }

    /// Gives buffer `id` back to the kernel (at the next `finishCompletions()`). An entry's fields are written one by
    /// one: the first entry's last two bytes are the ring's tail.
    @inlinable @inline(__always) public mutating func recycle(_ id: UInt16) {
        let entry = bufferRing + Int(bufferTail & UInt16(bufferCount - 1)) * ABI.bufferEntrySize
        entry.storeBytes(of: UInt64(UInt(bitPattern: bufferBase + Int(id) * bufferSize)), toByteOffset: ABI.bufferAddress, as: UInt64.self)
        entry.storeBytes(of: UInt32(bufferSize), toByteOffset: ABI.bufferLength, as: UInt32.self)
        entry.storeBytes(of: id, toByteOffset: ABI.bufferID, as: UInt16.self)
        bufferTail &+= 1
    }

    /// A sparse table of `count` direct descriptors (IORING_REGISTER_FILES2).
    private static func registerFiles(_ fd: Int32, count: UInt32) throws(SystemError) {
        let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.resourceRegSize, alignment: 8)
        defer { registration.deallocate() }
        registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.resourceRegSize)
        registration.storeBytes(of: count, toByteOffset: ABI.resourceRegCount, as: UInt32.self)
        registration.storeBytes(of: ABI.resourceRegisterSparse, toByteOffset: ABI.resourceRegFlags, as: UInt32.self)
        guard sys_syscall(ABI.sysRegister, Int(fd), ABI.registerFiles2, Int(bitPattern: registration), ABI.resourceRegSize, 0, 0) == 0 else {
            throw SystemError("io_uring_register files")
        }
    }
}

extension IORing {
    /// A ring's mappings, unmapped when it goes.
    private struct Maps {
        let sq: UnsafeMutableRawPointer
        let sqSize: Int
        /// The same as `sq` with IORING_FEAT_SINGLE_MMAP.
        let cq: UnsafeMutableRawPointer
        let cqSize: Int
        let sqes: UnsafeMutableRawPointer
        let sqesSize: Int

        func unmap() {
            munmap(sqes, sqesSize)
            if cq != sq { munmap(cq, cqSize) }
            munmap(sq, sqSize)
        }
    }

    /// io_uring_setup and the rings' mappings, laid out as the kernel's offsets say.
    private struct Rings {
        let fd: Int32
        let maps: Maps
        let sqHead, sqTail, sqFlags, cqHead, cqTail: UnsafeMutablePointer<Atomic<UInt32>>
        let sqMask, sqEntries, cqMask: UInt32
        let sqes: UnsafeMutableRawPointer
        let cqes: UnsafeMutableRawPointer

        init(_ entries: UInt32, _ completionEntries: UInt32, _ flags: UInt32) throws(SystemError) {
            let params = UnsafeMutableRawPointer.allocate(byteCount: ABI.paramsSize, alignment: 8)
            defer { params.deallocate() }
            params.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.paramsSize)
            params.storeBytes(of: completionEntries, toByteOffset: ABI.paramsCQEntries, as: UInt32.self)
            params.storeBytes(of: flags | ABI.setupCQSize, toByteOffset: ABI.paramsFlags, as: UInt32.self)
            let fd = Int32(truncatingIfNeeded: sys_syscall(ABI.sysSetup, Int(entries), Int(bitPattern: params), 0, 0, 0, 0))
            guard fd >= 0 else { throw SystemError("io_uring_setup") }

            let sqEntries = params.load(fromByteOffset: ABI.paramsSQEntries, as: UInt32.self)
            let cqEntries = params.load(fromByteOffset: ABI.paramsCQEntries, as: UInt32.self)
            let features = params.load(fromByteOffset: ABI.paramsFeatures, as: UInt32.self)
            let sq = { (field: Int) in Int(params.load(fromByteOffset: ABI.paramsSQOffsets + field, as: UInt32.self)) }
            let cq = { (field: Int) in Int(params.load(fromByteOffset: ABI.paramsCQOffsets + field, as: UInt32.self)) }
            var sqSize = sq(ABI.ringArray) + Int(sqEntries) * 4
            var cqSize = cq(ABI.ringCQEs) + Int(cqEntries) * ABI.cqeSize
            let single = features & ABI.featSingleMmap != 0
            if single {
                sqSize = max(sqSize, cqSize)
                cqSize = sqSize
            }
            func map(_ size: Int, _ offset: Int) -> UnsafeMutableRawPointer? {
                let p = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, off_t(offset))
                return p == UnsafeMutableRawPointer(bitPattern: -1) ? nil : p
            }
            guard let sqMap = map(sqSize, ABI.offSQRing) else {
                let error = SystemError("mmap io_uring")
                Glibc.close(fd)
                throw error
            }
            guard let cqMap = single ? sqMap : map(cqSize, ABI.offCQRing) else {
                let error = SystemError("mmap io_uring")
                munmap(sqMap, sqSize)
                Glibc.close(fd)
                throw error
            }
            let sqesSize = Int(sqEntries) * ABI.sqeSize
            guard let sqes = map(sqesSize, ABI.offSQEs) else {
                let error = SystemError("mmap io_uring")
                if !single { munmap(cqMap, cqSize) }
                munmap(sqMap, sqSize)
                Glibc.close(fd)
                throw error
            }
            func field(_ base: UnsafeMutableRawPointer, _ offset: Int) -> UnsafeMutablePointer<Atomic<UInt32>> {
                (base + offset).assumingMemoryBound(to: Atomic<UInt32>.self)
            }
            self.fd = fd
            maps = Maps(sq: sqMap, sqSize: sqSize, cq: cqMap, cqSize: cqSize, sqes: sqes, sqesSize: sqesSize)
            sqHead = field(sqMap, sq(ABI.ringHead))
            sqTail = field(sqMap, sq(ABI.ringTail))
            sqFlags = field(sqMap, sq(ABI.ringFlags))
            sqMask = sqMap.load(fromByteOffset: sq(ABI.ringMask), as: UInt32.self)
            self.sqEntries = sqMap.load(fromByteOffset: sq(ABI.ringEntries), as: UInt32.self)
            self.sqes = sqes
            if flags & ABI.setupNoSQArray == 0 {
                // The index array maps each place in the queue to the entry at the same place.
                let array = (sqMap + sq(ABI.ringArray)).assumingMemoryBound(to: UInt32.self)
                for i in 0..<Int(self.sqEntries) { array[i] = UInt32(i) }
            }
            cqHead = field(cqMap, cq(ABI.ringHead))
            cqTail = field(cqMap, cq(ABI.ringTail))
            cqMask = cqMap.load(fromByteOffset: cq(ABI.ringMask), as: UInt32.self)
            cqes = cqMap + cq(ABI.ringCQEs)
        }

        func unmap() {
            maps.unmap()
            Glibc.close(fd)
        }
    }

    /// A registered ring of provided buffers (IORING_REGISTER_PBUF_RING), group 0, and the buffers.
    private struct Buffers {
        let ring: UnsafeMutableRawPointer
        let base: UnsafeMutableRawPointer

        init(ringFD: Int32, count: UInt32, size: UInt32) throws(SystemError) {
            let ringSize = Int(count) * ABI.bufferEntrySize
            guard let ring = mmap(nil, ringSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0),
                  ring != UnsafeMutableRawPointer(bitPattern: -1) else { throw SystemError("mmap buffer ring") }
            let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.bufferRegSize, alignment: 8)
            defer { registration.deallocate() }
            registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.bufferRegSize)
            registration.storeBytes(of: UInt64(UInt(bitPattern: ring)), toByteOffset: ABI.bufferRegRing, as: UInt64.self)
            registration.storeBytes(of: count, toByteOffset: ABI.bufferRegEntries, as: UInt32.self)
            registration.storeBytes(of: ABI.bufferGroup, toByteOffset: ABI.bufferRegGroup, as: UInt16.self)
            let register = { sys_syscall(ABI.sysRegister, Int(ringFD), ABI.registerBufferRing, Int(bitPattern: registration), 1, 0, 0) }
            var rc = register()
            if rc < 0 && errno == EINVAL {
                // Ubuntu's 6.8.0-136 and later kernels refuse reserved words that are all zero (Launchpad #2162843, an
                // inverted check), and take a nonzero one: the first of 6.8's three 8-byte reserved words. Kernels
                // without the bug take the zeros above, so they never see this.
                registration.storeBytes(of: UInt64(1), toByteOffset: ABI.bufferRegReserved68, as: UInt64.self)
                rc = register()
            }
            guard rc == 0 else {
                let error = SystemError("io_uring_register buffer ring")
                munmap(ring, ringSize)
                throw error
            }
            guard let base = aligned_alloc(4096, Int(count) * Int(size)) else {
                munmap(ring, ringSize)
                throw SystemError("buffers", ENOMEM)
            }
            self.ring = ring
            self.base = base
        }
    }

    /// The kernel's io_uring interface (linux/io_uring.h): system call numbers, flags, opcodes, and the byte offsets of
    /// the fields the server uses. The same on x86-64 and arm64; checked against the header on both.
    @usableFromInline enum ABI {
        @usableFromInline static let sysSetup = 425, sysEnter = 426, sysRegister = 427

        @usableFromInline static let setupCQSize: UInt32 = 1 << 3, setupSubmitAll: UInt32 = 1 << 7, setupTaskrunFlag: UInt32 = 1 << 9
        @usableFromInline static let setupSingleIssuer: UInt32 = 1 << 12, setupDeferTaskrun: UInt32 = 1 << 13
        @usableFromInline static let setupNoSQArray: UInt32 = 1 << 16
        @usableFromInline static let featSingleMmap: UInt32 = 1 << 0
        @usableFromInline static let offSQRing = 0, offCQRing = 0x800_0000, offSQEs = 0x1000_0000
        @usableFromInline static let enterGetEvents: UInt32 = 1 << 0, enterExtArg: UInt32 = 1 << 3
        @usableFromInline static let sqCQOverflow: UInt32 = 1 << 1, sqTaskrun: UInt32 = 1 << 2
        @usableFromInline static let cqeBuffer: UInt32 = 1 << 0, cqeMore: UInt32 = 1 << 1, cqeBufferShift: UInt32 = 16

        @usableFromInline static let opAccept: UInt8 = 13, opAsyncCancel: UInt8 = 14, opClose: UInt8 = 19, opRead: UInt8 = 22
        @usableFromInline static let opSend: UInt8 = 26, opReceive: UInt8 = 27, opShutdown: UInt8 = 34
        @usableFromInline static let sqeFixedFile: UInt8 = 1 << 0, sqeHardLink: UInt8 = 1 << 3, sqeBufferSelect: UInt8 = 1 << 5
        @usableFromInline static let acceptMultishot: UInt16 = 1 << 0, receiveMultishot: UInt16 = 1 << 1
        @usableFromInline static let fileIndexAlloc = UInt32.max
        /// The buffer group the receives take from.
        @usableFromInline static let bufferGroup: UInt16 = 0
        // The socket flags and the shutdown's direction (sys/socket.h).
        @usableFromInline static let messageDontWait: UInt32 = 0x40, messageWaitAll: UInt32 = 0x100, messageNoSignal: UInt32 = 0x4000
        @usableFromInline static let shutdownBoth: UInt32 = 2

        @usableFromInline static let registerFiles2 = 13, registerBufferRing = 22, registerUnregisterBufferRing = 23
        @usableFromInline static let resourceRegisterSparse: UInt32 = 1 << 0

        // io_uring_params, and the io_sqring_offsets and io_cqring_offsets in it.
        @usableFromInline static let paramsSize = 120, paramsSQEntries = 0, paramsCQEntries = 4, paramsFlags = 8, paramsFeatures = 20
        @usableFromInline static let paramsSQOffsets = 40, paramsCQOffsets = 80
        @usableFromInline static let ringHead = 0, ringTail = 4, ringMask = 8, ringEntries = 12, ringFlags = 16, ringArray = 24, ringCQEs = 20
        // io_uring_sqe (64 bytes) and io_uring_cqe (16).
        @usableFromInline static let sqeSize = 64, cqeSize = 16
        @usableFromInline static let cqeUserData = 0, cqeResult = 8, cqeFlags = 12
        // io_uring_buf (16 bytes; the ring's tail is the first one's last two), io_uring_buf_reg (40),
        // io_uring_rsrc_register (32), io_uring_getevents_arg (24, its timespec pointer at 16).
        @usableFromInline static let bufferEntrySize = 16, bufferAddress = 0, bufferLength = 8, bufferID = 12, bufferRingTail = 14
        @usableFromInline static let bufferRegSize = 40, bufferRegRing = 0, bufferRegEntries = 8, bufferRegGroup = 12
        @usableFromInline static let bufferRegReserved68 = 16
        @usableFromInline static let resourceRegSize = 32, resourceRegCount = 0, resourceRegFlags = 4
        @usableFromInline static let getEventsArgSize = 24, getEventsArgTimespec = 16
    }
}

/// A submission entry's fields, at io_uring_sqe's offsets.
extension UnsafeMutableRawPointer {
    @usableFromInline var opcode: UInt8 {
        get { load(fromByteOffset: 0, as: UInt8.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 0, as: UInt8.self) }
    }
    @usableFromInline var sqeFlags: UInt8 {
        get { load(fromByteOffset: 1, as: UInt8.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 1, as: UInt8.self) }
    }
    @usableFromInline var ioprio: UInt16 {
        get { load(fromByteOffset: 2, as: UInt16.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 2, as: UInt16.self) }
    }
    @usableFromInline var fd: Int32 {
        get { load(fromByteOffset: 4, as: Int32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 4, as: Int32.self) }
    }
    @usableFromInline var offset: UInt64 {
        get { load(fromByteOffset: 8, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 8, as: UInt64.self) }
    }
    @usableFromInline var address: UInt64 {
        get { load(fromByteOffset: 16, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 16, as: UInt64.self) }
    }
    @usableFromInline var length: UInt32 {
        get { load(fromByteOffset: 24, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 24, as: UInt32.self) }
    }
    /// msg_flags (and the other per-opcode flags in the same union).
    @usableFromInline var opFlags: UInt32 {
        get { load(fromByteOffset: 28, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 28, as: UInt32.self) }
    }
    @usableFromInline var userData: UInt64 {
        get { load(fromByteOffset: 32, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 32, as: UInt64.self) }
    }
    @usableFromInline var bufferGroup: UInt16 {
        get { load(fromByteOffset: 40, as: UInt16.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 40, as: UInt16.self) }
    }
    @usableFromInline var fileIndex: UInt32 {
        get { load(fromByteOffset: 44, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 44, as: UInt32.self) }
    }
}
