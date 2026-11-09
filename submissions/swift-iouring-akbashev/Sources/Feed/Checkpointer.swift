// The one effect that leaves the main thread: copying the WAL back into the database file, whose fsync would otherwise
// stall the loop. An actor on a thread of its own at the lowest priority (nice 19), so on one CPU it runs while the
// loop waits, with a SQLite connection of its own. The loop starts a checkpoint without waiting for it, and learns that
// it finished from an eventfd its ring reads: a completion like any other. It also starts the WAL over when the loop
// asks, so the sync that comes with that is its, not the loop's.
import CSQLite
import Glibc
import Synchronization
import Syscalls

actor Checkpointer {
    nonisolated let executor: ThreadExecutor
    nonisolated var unownedExecutor: UnownedSerialExecutor { executor.asUnownedSerialExecutor() }

    private let db: OpaquePointer
    /// Written when a checkpoint is done: `copiedAll`, or 1 if frames are left.
    private let done: Int32
    static let copiedAll: UInt64 = 2
    /// The database's `user_version`, which `restartWAL` writes back as it is.
    private let userVersion: Int64

    init(path: String, done: Int32) throws(StoreError) {
        db = try SQLite.open(path, cacheKiB: 8 * 1024)
        userVersion = Self.userVersion(db)
        self.done = done
        executor = ThreadExecutor(name: "checkpoint", nice: 19)
    }

    /// A PASSIVE checkpoint: it copies what no reader still needs and never waits for the loop's transaction.
    func checkpoint() {
        var logged: Int32 = -1
        var copied: Int32 = -2
        let rc = sqlite3_wal_checkpoint_v2(db, nil, SQLITE_CHECKPOINT_PASSIVE, &logged, &copied)
        var result: UInt64 = rc == SQLITE_OK && logged == copied ? Self.copiedAll : 1
        _ = sys_write(done, &result, 8)
    }

    /// Starts the WAL over, with every frame copied and the loop's writes waiting: a commit that changes nothing
    /// (`user_version` set to what it is) is the first in the fresh WAL, so the sync of its header, which
    /// synchronous=NORMAL makes when a WAL starts over, happens on this thread, not the loop's. If a reader still
    /// holds the old WAL the commit just goes after it, and the next round tries again.
    func restartWAL() {
        let rc = sqlite3_exec(db, "PRAGMA user_version = \(userVersion)", nil, nil, nil)
        var result: UInt64 = rc == SQLITE_OK ? Self.copiedAll : 1
        _ = sys_write(done, &result, 8)
    }

    private static func userVersion(_ db: OpaquePointer) -> Int64 {
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nil) == SQLITE_OK, let stmt else { return 0 }
        defer { sqlite3_finalize(stmt) }
        return sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0
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
