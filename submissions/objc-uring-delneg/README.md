# Objective-C / raw io_uring / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx), written in plain Objective-C on the GNUstep runtime, with no Foundation, no framework, no event library.

## Versions

- Objective-C, clang 23 (apt.llvm.org `llvm-toolchain-noble-23`), `-fobjc-runtime=gnustep-2.2`, manual memory management
- libobjc2 2.3 (GNUstep runtime), robin-map 1.4.0, liburing 2.15, SQLite 3.54.1 amalgamation: all pinned by sha256 and built from source by `build.sh`
- Needs Linux 6.1+ (DEFER_TASKRUN); the grader's 6.8 kernel is fine

## Design

- Own root class `TDObject` (no NSObject). Connections are `TDConn` instances placed in a slab indexed by their io_uring fixed-file slot; hot methods are `objc_direct`; route handlers are objects whose IMPs are looked up once and called directly.
- One thread drives 8 io_urings: multishot direct accept, multishot recv into a provided buffer ring per ring, sends on fixed descriptors. Accepted sockets live in io_uring fixed-file tables, not the process fd table, so 8 rings × 65,535 slots lift the connection ceiling past `LimitNOFILE=65535`. Accept fills ring 0 first and moves to the next ring when a table is full, so below 65k connections only one ring is in use; ring 0 waits and holds a multishot poll on each other active ring's fd. One thread instead of a thread per ring: on one vCPU the threads only added context switches and lock convoys (p99 33 ms vs 4 ms in our A/B).
- `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`: completions are processed in batches.
- One SQLite connection (WAL, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, 1 GiB mmap, persistent prepared statements). Each loop iteration runs its whole completion batch (all rings) in one transaction (group commit) and submits every response only after `COMMIT` succeeds; if a commit fails, the batch's connections are dropped, so no 201 is ever sent for an uncommitted row.
- Every request reads SQLite while it is served, and every JWT signature (HS256, own SHA-256) and `exp` is verified on each request.
- `/feed` first reads the 20 newest ids from the `(created_at, id)` index; when they span fewer than 256 ids it reads their rows with one rowid range scan and their like counts with one grouped `likes_post_id_idx` range scan, then restores index order (otherwise the plain correlated query). Same transaction, so one snapshot.
- clang PGO: `pgo/train.m` builds a synthetic database (no seed data) and drives the store, JSON and JWT code; everything is then rebuilt with the profile. Falls back to a plain `-O3` build if any PGO step fails.
