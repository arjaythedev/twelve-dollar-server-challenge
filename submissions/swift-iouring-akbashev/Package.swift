// swift-tools-version:6.2
import PackageDescription

let package = Package(
    name: "feed",
    // Linux only (io_uring, eventfd). The one C target is SQLite's amalgamation, which build.sh fetches (pinned,
    // sha256-checked) and which compiles unmodified, its internal functions given external linkage so that BTree can
    // read SQLite's B-trees directly. io_uring is Swift over its system calls.
    targets: [
        .target(
            name: "CSQLite",
            path: "Sources/CSQLite",
            cSettings: [
                // SQLite's internals (`SQLITE_PRIVATE`, otherwise `static`), which BTree declares with `@_extern(c)`.
                .define("SQLITE_PRIVATE", to: ""),
                // Two connections, each used by one thread (the loop's, the checkpointer's): multi-thread mode. No
                // memory statistics, no features the server doesn't use.
                .define("SQLITE_THREADSAFE", to: "2"),
                .define("SQLITE_DEFAULT_MEMSTATUS", to: "0"),
                .define("SQLITE_DEFAULT_WAL_SYNCHRONOUS", to: "1"),
                .define("SQLITE_DQS", to: "0"),
                .define("SQLITE_LIKE_DOESNT_MATCH_BLOBS"),
                .define("SQLITE_MAX_EXPR_DEPTH", to: "0"),
                .define("SQLITE_OMIT_DEPRECATED"),
                .define("SQLITE_OMIT_LOAD_EXTENSION"),
                .define("SQLITE_OMIT_PROGRESS_CALLBACK"),
                .define("SQLITE_OMIT_SHARED_CACHE"),
                .define("SQLITE_OMIT_AUTOINIT"),
                .define("SQLITE_OMIT_UTF16"),
                .define("SQLITE_OMIT_TRACE"),
                .define("SQLITE_OMIT_COMPLETE"),
                .define("SQLITE_OMIT_GET_TABLE"),
                .define("SQLITE_OMIT_TCL_VARIABLE"),
                .define("SQLITE_USE_ALLOCA"),
                .define("HAVE_FDATASYNC", to: "1"),
                // Growing the database file by a chunk is one posix_fallocate, not a byte written into every block.
                .define("HAVE_POSIX_FALLOCATE", to: "1"),
                .define("HAVE_USLEEP", to: "1"),
                .unsafeFlags(["-O3", "-Wno-everything"]),
            ]
        ),
        // Cursors over SQLite's B-trees and the record format, for the read queries.
        .target(
            name: "BTree",
            dependencies: ["CSQLite"],
            path: "Sources/BTree",
            swiftSettings: [
                .enableExperimentalFeature("Lifetimes"),
                .enableExperimentalFeature("Extern"),
            ]
        ),
        // The Linux calls Glibc's module leaves out, declared in Swift with `@_extern(c)`, and the thin Swift helpers
        // over them (sockets, the descriptor limit, clocks, the log).
        .target(
            name: "Syscalls",
            path: "Sources/Syscalls",
            swiftSettings: [
                .enableExperimentalFeature("Extern"),
            ]
        ),
        // io_uring over its system calls: the rings, requests and completions. The per-request calls are inlinable, so
        // the reactor's compile as its own.
        .target(
            name: "IORing",
            dependencies: ["Syscalls"],
            path: "Sources/IORing"
        ),
        .executableTarget(
            name: "Feed",
            dependencies: ["CSQLite", "BTree", "IORing", "Syscalls"],
            path: "Sources/Feed",
            swiftSettings: [
                // The server is one thread: its state is the main actor's. The value types that hold no shared state
                // (bytes, JSON, HTTP, buffers, the store) are declared nonisolated, so code in them carries no main-actor
                // checks; the ring and the system helpers live in modules of their own, nonisolated throughout.
                .defaultIsolation(MainActor.self),
                .enableUpcomingFeature("NonisolatedNonsendingByDefault"),
                .enableUpcomingFeature("InferIsolatedConformances"),
                .enableUpcomingFeature("ExistentialAny"),
                .enableUpcomingFeature("MemberImportVisibility"),
                // `@_lifetime` on the ~Escapable views (an HTTP request, the JSON parser) and on the Spans the
                // noncopyable buffers lend out.
                .enableExperimentalFeature("Lifetimes"),
            ]
        ),
    ]
)
