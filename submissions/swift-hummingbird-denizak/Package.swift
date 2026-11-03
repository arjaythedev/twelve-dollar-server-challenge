// swift-tools-version:6.1
import PackageDescription

let package = Package(
    name: "twelve-dollar-server",
    platforms: [.macOS(.v14)],
    dependencies: [
        // Pinned: every dependency version in Package.resolved must build with the
        // Swift 6.1.2 toolchain installed by install.sh (tools-version <= 6.1).
        .package(url: "https://github.com/hummingbird-project/hummingbird.git", exact: "2.26.0"),
        .package(url: "https://github.com/apple/swift-crypto.git", exact: "3.12.3"),
        // Pinned: 1.7.2+ require a swift-tools-version 6.4 toolchain.
        .package(url: "https://github.com/apple/swift-collections.git", exact: "1.7.1"),
    ],
    targets: [
        // System libsqlite3 (installed by install.sh: libsqlite3-dev).
        .systemLibrary(
            name: "CSQLite3",
            path: "Sources/CSQLite3",
            providers: [.apt(["libsqlite3-dev"])]
        ),
        .executableTarget(
            name: "server",
            dependencies: [
                .product(name: "Hummingbird", package: "hummingbird"),
                .product(name: "Crypto", package: "swift-crypto"),
                "CSQLite3",
            ],
            path: "Sources/App"
        ),
    ]
)
