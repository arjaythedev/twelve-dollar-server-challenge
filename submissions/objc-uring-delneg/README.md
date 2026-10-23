# Objective-C / raw io_uring / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx), written in plain Objective-C on the GNUstep runtime, with no Foundation, no framework, no event library.

## Versions

- Objective-C, clang 23 (apt.llvm.org `llvm-toolchain-noble-23`), `-fobjc-runtime=gnustep-2.2`, manual memory management
- libobjc2 2.3 (GNUstep runtime), robin-map 1.4.0, liburing 2.15, SQLite 3.54.1 amalgamation: all pinned by sha256 and built from source by `build.sh`
- Needs Linux 6.1+ (DEFER_TASKRUN); the grader's 6.8 kernel is fine

## Design

- Own root class `TDObject` (no NSObject). Connections are `TDConn` instances placed in a slab indexed by their io_uring fixed-file slot; hot methods are `objc_direct`; route handlers are objects whose IMPs are looked up once and called directly.
- One io_uring per worker thread, each with its own `SO_REUSEPORT` listener: multishot direct accept, multishot recv into a provided buffer ring, sends on fixed descriptors. Accepted sockets live in io_uring fixed-file tables, not the process fd table, so 8 rings × 65,535 slots lift the connection ceiling past `LimitNOFILE=65535`; a full table pauses accept instead of crashing.
- `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`: completions are processed in batches.
- One SQLite connection (WAL, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, 1 GiB mmap, persistent prepared statements) shared under a mutex. A worker runs a whole completion batch in one transaction (group commit) and submits every response only after `COMMIT` succeeds; if a commit fails, the batch's connections are dropped, so no 201 is ever sent for an uncommitted row.
- Every request reads SQLite while it is served, and every JWT signature (HS256, own SHA-256) and `exp` is verified on each request.
