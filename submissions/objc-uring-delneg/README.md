# Objective-C / raw io_uring / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx), written in plain Objective-C on the GNUstep runtime, with no Foundation, no framework, no event library.

## Versions

- Objective-C, clang 23.1.4 (official LLVM release tarball, pinned by sha256; `install.sh` unpacks only clang, lld, llvm-profdata and clang's runtime), `-fobjc-runtime=gnustep-2.2`, manual memory management
- libobjc2 2.3 (GNUstep runtime), robin-map 1.4.0, liburing 2.15, SQLite 3.54.1 amalgamation: all pinned by sha256 and built from source by `build.sh` (SQLite is stock, unpatched)
- Needs Linux 6.1+ (DEFER_TASKRUN); the grader's 6.8 kernel is fine. Ubuntu's 6.8.0-147 refuses a buffer-ring registration whose reserved words are all zero (an inverted check, Launchpad #2162843), so a refused registration is retried once with `resv[0]=1`

## Design

- Own root class `TDObject` (no NSObject). Connections are `TDConn` instances placed in a slab indexed by their io_uring fixed-file slot; hot methods are `objc_direct`; route handlers are objects whose IMPs are looked up once and called directly.
- One thread drives 8 io_urings: multishot direct accept, multishot recv into a provided buffer ring per ring, sends on fixed descriptors. Accepted sockets live in io_uring fixed-file tables, not the process fd table, so 8 rings × 65,535 slots lift the connection ceiling past `LimitNOFILE=65535`. Accept fills ring 0 first and moves to the next ring at a high-water mark 4,096 slots below full (a full table makes the kernel drop the socket it just accepted), so below ~61k connections only one ring is in use and none is dropped at a switch; ring 0 waits and holds a multishot poll on each other active ring's fd. One thread instead of a thread per ring: on one vCPU the threads only added context switches and lock convoys (p99 33 ms vs 4 ms in our A/B).
- Under load (8+ requests in the last pass) the loop waits for up to 128 completions or 1 ms before processing, so batches and group commits get bigger and `io_uring_enter` calls fewer (idea from #32).
- Every ring is `SINGLE_ISSUER | COOP_TASKRUN`, and ring 0 (the one the thread waits on) also `DEFER_TASKRUN`: completions are processed in batches.
- One SQLite connection (WAL, `synchronous=NORMAL`, no mmap (a 32 MB page cache measured ~3% faster than per-page mmap objects), persistent prepared statements). Each loop iteration runs its whole completion batch (all rings) in one transaction (group commit) and submits every response only after `COMMIT` succeeds; if a commit fails, the batch's connections are dropped, so no 201 is ever sent for an uncommitted row.
- Checkpoints run in a child process, forked at startup before any SQLite or io_uring state exists. The loop asks it (over a pipe, answer read through io_uring) for a `PASSIVE` checkpoint every 250 new WAL frames, so the copy and its two fsyncs never block the loop. Under constant writes a passive pass can never copy the whole WAL, so the WAL could not rewind and would grow without bound. When it reaches 8192 frames (~32 MB), the loop stops running batches (requests queue up) until one pass copies everything; the next write then rewinds the WAL. That pause is ~40–80 ms every ~1.5 s with an 11 ms-fsync disk, with a 250 ms cap. If the child dies, SQLite's own autocheckpoint takes over inline. The second process needs the shared-memory WAL index, so locking is `NORMAL`, not `EXCLUSIVE`: the extra file locks measured as noise (11.56 vs 11.54–11.60 µs/req).
- Every request reads SQLite while it is served, and every JWT signature (HS256, own SHA-256) and `exp` is verified on each request.
- `/feed` and `GET /posts/:id` read SQLite's B-tree layer through its own cursor calls, with no bytecode engine (`src/td_btree.c` includes the stock amalgamation, so `sqlite3BtreeCursor`, `First`, `Next`, `Previous`, `TableMoveto`, `IndexMoveto` and `PayloadFetch` are reachable). Pages still come through SQLite's pager, page cache, WAL and locks, inside the batch's transaction. `/feed` walks `posts_created_at_id_idx` from its first entry (the index is DESC, so newest first) for 20 ids, counts their likes with one `likes_post_id_idx` range scan when the ids span fewer than 256 (one seek per post otherwise), then seeks each post by rowid (one step back when ids are consecutive) and its author in `users`. Nothing is kept between requests except root page numbers, checked against the schema cookie on every call. A row the fast path does not cover (a record on overflow pages, an unexpected column type) sends the request to the SQL statements, which are still prepared (`--btree=0` uses them for every read).
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
| Checkpointer process with stop-the-world WAL rewind (see Design) | DB on a real disk, 40k users: 50.5k → 81.8k req/s served (p50 324 → 1.8 ms, p99 490 → 92 ms); in-loop checkpoint fsyncs had made the server disk-bound. tmpfs: CPU equal |
| `INSERT` without `RETURNING` (`RETURNING` buffers its rows in an ephemeral table, a page cache set up and torn down per insert); `created_at` written in the schema default's format, id from `sqlite3_last_insert_rowid` | create-only wrk: 85–89k → 104k req/s (11.5 → 9.6 µs/req); realistic mix −0.5% CPU |
| Feed like counts tallied in C from a plain `post_id` index range scan instead of `GROUP BY post_id`; like insert binds `created_at` (no per-row `strftime`) | feed-only wrk 12.6 → 11.5 µs/req (+10%), like-only 5.7 → 5.26 µs/req (+7%); realistic mix −1% CPU |
| Feed likes tallied by an app-defined aggregate, `SELECT td_tally(post_id) FROM likes WHERE post_id BETWEEN ?1 AND ?2`, inside one `sqlite3_step` instead of one returned row per like (statement: 3.69 → 2.56 µs for 87 likes) | feed-only wrk 10.7 → 10.3 µs/req (−3.5%), wrk mix −1.5%; realistic mix −0.2% |
| JSON string escaper checks 8 bytes at a time (SWAR test for `< 0x20`, `"`, `\`) before falling back to bytes | feed-only wrk 11.4 → 10.6 µs/req (+7%); realistic mix −0.5% CPU |
| `/feed` and `GET /posts/:id` on SQLite's B-tree layer (see Design) instead of three prepared statements; measured on a Synteq 8 vCPU box like the grader's (server on 1 CPU / 2 GB in Docker, kload with the `bench/load.js` model on 4 other CPUs) | 300,000 users: 26.54 and 26.04 µs/req against 28.49 (and 27.54–29.14 in three baseline runs), CPU 83 % against 90 %. Saturated wrk: `/feed` −21 % CPU per request (127k → 85k instructions), `GET /posts/:id` −6.5 % |

Final (`e23849b`, still with the SQLite patch that was later removed, see below): wrk mix 6.80–6.85 µs/req, about 146k rps. This was measured after an opt-box reboot that shifted every number (the baseline went from 9.9 to 12.1 µs/req on the userload metric), so compare it only with runs from the same session. Before the round-2 changes (`4db6b5a`): about 131k rps at 7.6 µs/req; per route `/health` 3.2 µs (kernel and loop floor), `GET /posts/:id` 4.0, like 5.9, create 12.1, `/feed` 12.6. For reference, Rust io_uring #82 built from its own scripts on the same harness: 115k rps, 8.7 µs/req (this entry at that time: 127k, 7.9 µs/req).

Did not help (reverted): bigger `wal_autocheckpoint` (4096: −4% CPU on tmpfs, double the p99 on disk); response header written into the body's headroom (+0.5%, noise); AVX2 scan in the JSON escaper (−2%: bodies are short); streaming feed rows without the row copy (noise); a 256 MB page cache (more CPU than 32 MB); extra SQLite `OMIT_*`/`UNTESTABLE` compile options (noise); PGO that also profiles the running server over loopback (noise).

Removed after review: a one-line patch to the SQLite amalgamation (`balance_nonroot` parked a page under `nPage+1` instead of the pending-byte page number; create-only wrk +8%, realistic mix −0.5% CPU). Arjay ruled on 2026-10-31 that the engine has to be stock SQLite (rule 2), so build.sh no longer edits it.

Memory: at 100,000 idle connections the cgroup holds 407 MB, of which 20 MB is the process (anon) and the rest kernel socket and io_uring memory (~3.9 KB per connection), so 2 GB holds more connections than the 8 × 65,535 slots.
