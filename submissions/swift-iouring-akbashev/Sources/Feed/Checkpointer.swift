// The one effect that leaves the main thread: copying the WAL back into the database file, whose fsync would otherwise
// stall the loop. An actor on a thread of its own at a low priority (nice 5), so on one CPU it mostly runs while the
// loop waits, with a SQLite connection of its own that only copies: it never commits, since a commit from another
// connection makes SQLite drop the loop connection's page cache. The loop starts a checkpoint without waiting for it,
// and learns that it finished from an eventfd its ring reads: a completion like any other.
import CSQLite
import Glibc
import Synchronization
import Syscalls

actor Checkpointer {
    nonisolated let executor: ThreadExecutor
    nonisolated var unownedExecutor: UnownedSerialExecutor { executor.asUnownedSerialExecutor() }

    private let db: OpaquePointer
    /// The database file, for syncing it before writes are held.
    private let file: Int32
    /// Written when a checkpoint is done: `copiedAll`, or 1 if frames are left.
    private let done: Int32
    static let copiedAll: UInt64 = 2

    init(path: String, done: Int32) throws(StoreError) {
        db = try SQLite.open(path, cacheKiB: 8 * 1024)
        file = Glibc.open(path, O_RDONLY | O_CLOEXEC)
        self.done = done
        // At nice 19 it was starved exactly when the loop was saturated, so a hold ran out before the copy did; at
        // nice 0 it takes half the CPU from the loop whenever both run. At 5 it gets about a quarter of a contended CPU.
        executor = ThreadExecutor(name: "checkpoint", nice: 5)
    }

    /// PASSIVE checkpoints: they copy what no reader still needs and never wait for the loop's transaction.
    ///
    /// `bulk`, while writes go on: up to four copies while more than 128 frames are left, then the database file
    /// synced, one more copy, and another sync. A copy that doesn't finish the WAL writes its pages without syncing
    /// them (SQLite syncs the file only once every frame is in), so the sync at the end of the held copy would flush
    /// the whole cycle's pages while writes wait; synced here, the held copy is a few dozen frames. Otherwise (writes
    /// held) one copy, which syncs.
    func checkpoint(bulk: Bool) {
        var logged: Int32 = -1
        var copied: Int32 = -2
        var rc = SQLITE_OK
        for _ in 0..<(bulk ? 4 : 1) {
            rc = sqlite3_wal_checkpoint_v2(db, nil, SQLITE_CHECKPOINT_PASSIVE, &logged, &copied)
            if rc != SQLITE_OK || logged - copied <= 128 { break }
        }
        if bulk && rc == SQLITE_OK {
            if file >= 0 { _ = fdatasync(file) }
            rc = sqlite3_wal_checkpoint_v2(db, nil, SQLITE_CHECKPOINT_PASSIVE, &logged, &copied)
            if file >= 0 { _ = fdatasync(file) }
        }
        var result: UInt64 = rc == SQLITE_OK && logged == copied ? Self.copiedAll : 1
        _ = sys_write(done, &result, 8)
    }
}

/// A serial executor with a thread of its own, for an actor that is an effect: jobs queue under a Mutex, and the thread
/// sleeps in a blocking read of an eventfd that `enqueue` writes.
nonisolated final class ThreadExecutor: SerialExecutor, TaskExecutor {
    private let jobs = Mutex<[UnownedJob]>([])
    private let wake: Int32
    private let threadID = Atomic<UInt>(0)

    init(name: String, nice: Int32) {
        wake = sys_eventfd(0, Sys.EFD_CLOEXEC)
        guard wake >= 0 else { fatalError("eventfd: \(errno)") }
        Self.startThread(name: name) { [self] in
            // This thread only: the main thread keeps its priority.
            _ = sys_setpriority(0, UInt32(sys_gettid()), nice)
            run()
        }
    }

    func enqueue(_ job: consuming ExecutorJob) {
        let job = UnownedJob(job)
        jobs.withLock { $0.append(job) }
        var one: UInt64 = 1
        _ = sys_write(wake, &one, 8)
    }

    func asUnownedSerialExecutor() -> UnownedSerialExecutor {
        UnownedSerialExecutor(ordinary: self)
    }

    func isIsolatingCurrentContext() -> Bool? {
        threadID.load(ordering: .relaxed) == UInt(bitPattern: Int(bitPattern: pthread_self()))
    }

    func checkIsolated() {
        if isIsolatingCurrentContext() != true { fatalError("not on this executor's thread") }
    }

    private func run() -> Never {
        threadID.store(UInt(bitPattern: Int(bitPattern: pthread_self())), ordering: .relaxed)
        var batch: [UnownedJob] = []
        while true {
            jobs.withLock { swap(&batch, &$0) }
            if batch.isEmpty {
                var value: UInt64 = 0
                _ = sys_read(wake, &value, 8)
                continue
            }
            for job in batch {
                job.runSynchronously(isolatedTo: asUnownedSerialExecutor(), taskExecutor: asUnownedTaskExecutor())
            }
            batch.removeAll(keepingCapacity: true)
        }
    }

    /// A thread's body on its way through pthread_create.
    private final class ThreadStart: Sendable {
        let name: String
        let body: @Sendable () -> Void
        init(name: String, body: @escaping @Sendable () -> Void) {
            self.name = name
            self.body = body
        }
    }

    private static func startThread(name: String, _ body: @escaping @Sendable () -> Void) {
        let start = Unmanaged.passRetained(ThreadStart(name: name, body: body)).toOpaque()
        var thread = pthread_t()
        let rc = pthread_create(&thread, nil, { raw in
            let start = Unmanaged<ThreadStart>.fromOpaque(raw!).takeRetainedValue()
            _ = sys_pthread_setname_np(pthread_self(), String(start.name.prefix(15)))
            start.body()
            return nil
        }, start)
        if rc != 0 { fatalError("pthread_create: \(rc)") }
        pthread_detach(thread)
    }
}
