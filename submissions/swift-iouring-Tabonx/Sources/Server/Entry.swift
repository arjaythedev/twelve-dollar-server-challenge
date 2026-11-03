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
        func runEpoll(_ reason: StaticString) -> Never {
            _ = write(2, reason.utf8Start, reason.utf8CodeUnitCount)
            guard var server = EpollServer(service: service, host: host, port: port) else { fail("cannot listen") }
            server.run()
        }
        if tu_probe() == 0 || environment("SERVER_FORCE_EPOLL") != nil {
            runEpoll("io_uring unavailable, using epoll\n")
        }
        sv_ignore_sigpipe()
        // Each ring's registered file table is capped by RLIMIT_NOFILE; more rings hold more connections.
        let slots = Int(min(sv_raise_nofile(), 65535))
        var listeners: [Int32] = []
        for _ in 0..<listenerCount {
            let fd = sv_listen(host, port, 65535, 1)
            guard fd >= 0 else { fail("cannot listen") }
            listeners.append(fd)
        }
        guard let server = Server(service: service, listeners: listeners, slots: slots) else {
            for fd in listeners { _ = close(fd) }
            runEpoll("io_uring setup failed, using epoll\n")
        }
        server.run()
    }
}
#else
@main
struct Main {
    static func main() { fatalError("this server runs on Linux only") }
}
#endif
