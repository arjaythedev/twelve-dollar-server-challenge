#if os(Linux)
import CShim
import CUring

func environment(_ name: StaticString) -> UnsafePointer<CChar>? {
    sv_getenv(UnsafeRawPointer(name.utf8Start).assumingMemoryBound(to: CChar.self))
}

func fail(_ message: StaticString) -> Never {
    _ = write(2, message.utf8Start, message.utf8CodeUnitCount)
    _ = write(2, "\n", 1)
    exit(1)
}

/// What a worker thread needs to build its ring. Rings are created on the thread that uses them
/// (IORING_SETUP_SINGLE_ISSUER).
final class WorkerConfig {
    let service: FeedService
    let listener: Int32
    let slots: Int
    init(service: FeedService, listener: Int32, slots: Int) {
        self.service = service
        self.listener = listener
        self.slots = slots
    }
}

func runWorker(_ config: WorkerConfig) -> Never {
    guard let w = Worker(service: config.service, listener: config.listener, slots: config.slots) else {
        fail("io_uring setup failed")
    }
    w.run()
}

/// Thread entry point. A global function, so it isn't isolated to the main actor.
func workerThread(_ raw: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    runWorker(Unmanaged<WorkerConfig>.fromOpaque(raw.unsafelyUnwrapped).takeUnretainedValue())
}

@main
struct Main {
    static func main() {
        guard let path = environment("SQLITE_PATH") else { fail("SQLITE_PATH is not set") }
        guard let secret = environment("JWT_SECRET") else { fail("JWT_SECRET is not set") }
        let host = environment("HOST") ?? UnsafeRawPointer(("0.0.0.0" as StaticString).utf8Start).assumingMemoryBound(to: CChar.self)
        let port: Int32 = environment("PORT").map { atoi($0) } ?? 80

        var error = OutBuffer(capacity: 256)
        let secretView = ByteView(base: UnsafeRawPointer(secret).assumingMemoryBound(to: UInt8.self), count: strlen(secret))
        guard let service = FeedService(sqlitePath: path, secret: secretView, error: &error) else {
            _ = write(2, error.base, error.count)
            fail(": cannot open the database")
        }
        if tu_probe() == 0 || environment("SERVER_FORCE_EPOLL") != nil {
            _ = write(2, "io_uring unavailable, using epoll\n", 34)
            guard var server = EpollServer(service: service, host: host, port: port) else { fail("cannot listen") }
            server.run()
        }
        sv_ignore_sigpipe()
        // Each ring's registered file table is capped by RLIMIT_NOFILE; more rings hold more connections.
        let slots = Int(min(sv_raise_nofile(), 65535))
        let listener = sv_listen(host, port, 65535)
        guard listener >= 0 else { fail("cannot listen") }
        var rings = 8
        if let r = environment("SERVER_RINGS") { rings = max(1, Int(atoi(r))) }

        let config = WorkerConfig(service: service, listener: listener, slots: slots)
        for _ in 1..<rings {
            _ = tu_spawn(workerThread, Unmanaged.passRetained(config).toOpaque())
        }
        runWorker(config)
    }
}
#else
@main
struct Main {
    static func main() { fatalError("this server runs on Linux only") }
}
#endif
