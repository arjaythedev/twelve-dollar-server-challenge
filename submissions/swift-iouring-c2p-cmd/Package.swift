// swift-tools-version:6.0
import PackageDescription

// build.sh fetches the pinned SQLite amalgamation into vendor/ and builds liburing 2.15 into
// .deps/uring (headers in .deps/uring/include); both stay out of the Swift module's include/.
let sqliteDefines: [CSetting] = [
    .define("SQLITE_THREADSAFE", to: "2"),
    .define("SQLITE_DEFAULT_MEMSTATUS", to: "0"),
    .define("SQLITE_DEFAULT_WAL_SYNCHRONOUS", to: "1"),
    .define("SQLITE_DQS", to: "0"),
    .define("SQLITE_LIKE_DOESNT_MATCH_BLOBS"),
    .define("SQLITE_MAX_EXPR_DEPTH", to: "0"),
    .define("SQLITE_OMIT_DEPRECATED"),
    .define("SQLITE_OMIT_PROGRESS_CALLBACK"),
    .define("SQLITE_OMIT_SHARED_CACHE"),
    .define("SQLITE_OMIT_LOAD_EXTENSION"),
    .define("SQLITE_OMIT_AUTOINIT"),
    .define("SQLITE_OMIT_UTF16"),
    .define("SQLITE_OMIT_TRACE"),
    .define("SQLITE_OMIT_COMPLETE"),
    .define("SQLITE_OMIT_GET_TABLE"),
    .define("SQLITE_OMIT_TCL_VARIABLE"),
    .define("SQLITE_USE_ALLOCA"),
    .define("HAVE_FDATASYNC", to: "1"),
    .define("HAVE_USLEEP", to: "1"),
]

let package = Package(
    name: "feed-server",
    targets: [
        .target(
            name: "CShim",
            path: "Sources/CShim",
            cSettings: sqliteDefines + [
                .headerSearchPath("../../vendor"),
                .headerSearchPath("../../.deps/uring/include"),
                .unsafeFlags(["-O3", "-march=native", "-fno-plt", "-w"]),
            ],
            linkerSettings: [.linkedLibrary("m"), .linkedLibrary("pthread")]
        ),
        .executableTarget(
            name: "feed-server",
            dependencies: ["CShim"],
            path: "Sources/Server",
            swiftSettings: [.unsafeFlags(["-Ounchecked", "-enforce-exclusivity=unchecked"])]
        ),
    ],
    swiftLanguageModes: [.v5]
)
