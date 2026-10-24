# Objective-C / raw io_uring / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx), written in plain Objective-C on the GNUstep runtime, with no Foundation, no framework, no event library.

## Versions

- Objective-C, clang 23 (apt.llvm.org `llvm-toolchain-noble-23`; the major version is pinned, apt.llvm.org only keeps the latest 23.x build), `-fobjc-runtime=gnustep-2.2`, manual memory management
- libobjc2 2.3 (GNUstep runtime), robin-map 1.4.0, liburing 2.15, SQLite 3.54.1 amalgamation: all pinned by sha256 and built from source by `build.sh`
- Needs Linux 6.1+ (DEFER_TASKRUN); the grader's 6.8 kernel is fine

## Design

- Own root class `TDObject` (no NSObject). Connections are `TDConn` instances placed in a slab indexed by their io_uring fixed-file slot; hot methods are `objc_direct`; route handlers are objects whose IMPs are looked up once and called directly.
- One thread drives 8 io_urings: multishot direct accept, multishot recv into a provided buffer ring per ring, sends on fixed descriptors. Accepted sockets live in io_uring fixed-file tables, not the process fd table, so 8 rings × 65,535 slots lift the connection ceiling past `LimitNOFILE=65535`. Accept fills ring 0 first and moves to the next ring at a high-water mark 4,096 slots below full (a full table makes the kernel drop the socket it just accepted), so below ~61k connections only one ring is in use and none is dropped at a switch; ring 0 waits and holds a multishot poll on each other active ring's fd. One thread instead of a thread per ring: on one vCPU the threads only added context switches and lock convoys (p99 33 ms vs 4 ms in our A/B).
- Under load (8+ requests in the last pass) the loop waits for up to 128 completions or 1 ms before processing, so batches and group commits get bigger and `io_uring_enter` calls fewer (idea from #32).
- Every ring is `SINGLE_ISSUER | COOP_TASKRUN`, and ring 0 (the one the thread waits on) also `DEFER_TASKRUN`: completions are processed in batches.
- One SQLite connection (WAL, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, no mmap (a 32 MB page cache measured ~3% faster than per-page mmap objects), persistent prepared statements). Each loop iteration runs its whole completion batch (all rings) in one transaction (group commit) and submits every response only after `COMMIT` succeeds; if a commit fails, the batch's connections are dropped, so no 201 is ever sent for an uncommitted row.
- Every request reads SQLite while it is served, and every JWT signature (HS256, own SHA-256) and `exp` is verified on each request.
- `/feed` first reads the 20 newest ids from the `(created_at, id)` index; when they span fewer than 256 ids it reads their rows with one rowid range scan and their like counts with one grouped `likes_post_id_idx` range scan, then restores index order (otherwise the plain correlated query). Same transaction, so one snapshot.
- clang PGO: `pgo/train.m` builds a synthetic database (no seed data) and drives the store, JSON and JWT code; everything is then rebuilt with the profile. Falls back to a plain `-O3` build if any PGO step fails.

## HTTP handling

Own parser: request line, `Content-Length`, `Authorization` and `Connection` are read; query strings are ignored. Keep-alive by default on HTTP/1.1, `Connection: close` and HTTP/1.0 (without `keep-alive`) close after the response. Pipelined requests are answered in order. A request larger than 16 KB, or one with `Transfer-Encoding`, is dropped without a response (no 201 can be sent for it). `Expect: 100-continue` gets no interim response; clients send the body after their timeout and get the normal answer.

## Optimizations, measured

Server in Docker capped at 1 CPU / 2 GB (`--cpuset-cpus=0`, host network), wrk with a `bench/load.js`-like mix (per cycle 1 feed, 1 post, 0.15 like, 0.02 create; reads and likes use ids from the last feed) on the other cores of a 4 vCPU i5-11400F VM. Metric: server cgroup CPU per request at saturation, interleaved A/B runs.

| Change | Result |
|---|---|
| Baseline: one thread per ring, spec feed query | 63.9k rps, 15.6 µs/req |
| `/feed` by id range scans (see Design) | +19% rps (15.6 → 13.1 µs/req) |
| clang PGO on a synthetic database | +10% rps (13.2 → 12.0 µs/req) |
| One thread drives all rings | +3% rps, p99 33 → 4 ms |
| Accept leaves a ring at a high-water mark | 140,000 held connections: 0 dropped (2 before) |
| No SQLite mmap | +3.4% rps (7.85 → 7.60 µs/req on the corrected mix) |
| Batching window: after a pass with 8+ requests, wait for up to 128 completions or 1 ms | −5.3% CPU per request at 42k req/s from 20k users (10.53 → 9.96 µs/req), p99 1.0 → 2.1 ms |
| `wal_autocheckpoint` 1000 → 250 pages | DB on a real disk (fsync ~11 ms), 42k req/s: p99 46 → 27 ms (the in-loop checkpoint's stalls get shorter); 100 is fsync-bound, 4096+ much worse; CPU equal |

Final: about 131k rps at 7.6 µs/req on the mix. Per route: `/health` 3.2 µs (kernel and loop floor), `GET /posts/:id` 4.0, like 5.9, create 12.1, `/feed` 12.6. For reference, Rust io_uring #82 built from its own scripts on the same harness: 115k rps, 8.7 µs/req (this entry before the mmap change: 127k, 7.9 µs/req).

Did not help (reverted): bigger `wal_autocheckpoint` (4096: −4% CPU on tmpfs, double the p99 on disk); response header written into the body's headroom (+0.5%, noise); AVX2 scan in the JSON escaper (−2%: bodies are short); streaming feed rows without the row copy (noise); a 256 MB page cache (more CPU than 32 MB); extra SQLite `OMIT_*`/`UNTESTABLE` compile options (noise); PGO that also profiles the running server over loopback (noise).

Memory: at 100,000 idle connections the cgroup holds 407 MB, of which 20 MB is the process (anon) and the rest kernel socket and io_uring memory (~3.9 KB per connection), so 2 GB holds more connections than the 8 × 65,535 slots.
