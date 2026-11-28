# Kotlin/Native, no framework: io_uring + SQLite B-tree reads

The Kotlin handlers of [kotlin-nio-3x3xX3N0N](../kotlin-nio-3x3xX3N0N) (HTTP parsing, routing, JWT, JSON, group
commit), compiled with Kotlin/Native for linuxX64 instead of the JVM, on one thread over io_uring, with SQLite
compiled in. The C side is kernel and SQLite plumbing only.

| | |
|---|---|
| Language | Kotlin 2.4.21, Kotlin/Native (`linuxX64`, `-opt`); the compiler runs on Temurin JDK 25.0.4.1 at build time only |
| Framework | none (`src/Server.kt`) |
| io_uring | liburing 2.9 (MIT), fetched by `build.sh` at its release tag, sha256-checked, called from `csrc/uring.c` |
| SQLite | 3.54.1 amalgamation, unchanged, SHA3-256-checked against sqlite.org, compiled into `csrc/db.c` |
| C compiler | the clang + glibc sysroot that ship with Kotlin/Native (so the C objects match its linker), profile-guided: trained in `build.sh` on a synthetic database |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.0+ io_uring (multishot receive, provided-buffer rings, direct descriptors). No epoll fallback |

## Design

**One thread, io_uring, no fds for clients.** `csrc/uring.c`:
- Accepted sockets go straight into a ring's *direct-descriptor* table (`accept_direct`, `IORING_FILE_INDEX_ALLOC`),
  so a connection never takes a normal fd. A table holds at most `RLIMIT_NOFILE` (65,535) entries, so when one ring
  is `FD_MARGIN` short of full, accepts move to the next ring (up to 8 rings, ~520k connections, one process).
  Secondary rings signal one eventfd that ring 0 reads, so the thread sleeps in a single `io_uring_enter`.
- **Batch wake**: after a light pass the loop waits on ring 0 alone for 256 completions or 1 ms (or the group-commit
  deadline), then collects every ring. At 160k users: 8.2k → 1.9k loop passes a second, −6-11 % CPU per request,
  reads p95 0.3 → 2.2 ms. (`DEFER_TASKRUN` is a knob, off: together with this wait it failed reads at 256k users.)
- `KN_PASS=sweep` (knob, off) is java-iouring-davidhsv's pass structure instead: every ring `DEFER_TASKRUN`; each
  pass first sweeps the secondary rings without waiting (submit, run their deferred completions; a second enter
  with a zero timeout if the kernel still flags task work), then waits on ring 0 for 256 completions or 1 ms after
  every non-empty pass, then collects every ring. Measured: −0.6 µs/req at 160k (3 pairs), but +1.4/+1.7 µs/req and
  reads p95 8 → 69 ms at 256k (2 pairs), so batch wake stays the default.
- 512 single-shot accepts in flight (multishot accept into a direct table lost connections for another entry).
- Multishot receive into a provided-buffer ring (4,096 × 2 KB per ring): an idle connection owns no buffer. Buffers go
  back after each pass. A receive that runs out of buffers (`ENOBUFS`) is re-armed once the pass's buffers are back.
- Each response is copied once into a send request; a short write resubmits the rest; output behind an in-flight send
  is queued so the order on the wire is kept. A `Connection: close` ends with `shutdown`, and the slot is closed when
  the receive sees the end.
- The soft `RLIMIT_NOFILE` is raised to the hard one at start. `TCP_NODELAY` and `TCP_DEFER_ACCEPT` on the listener.
  One listener by default; `KN_LISTENERS=8` opens 8 `SO_REUSEPORT` listeners (measured: same CPU, connect p99
  0.7-0.8 → 0.5 ms at 256k; not the default because a restart on Ubuntu's 6.8 kernel could share the port with the
  old process's listener while that one is torn down).
- The binary's own code and read-only data (~2.2 MB) are `mlock`ed at start, so near the memory wall reclaim cannot
  page the loop's code out (no swap on the box: only file pages can be reclaimed).
- Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero (an inverted check,
  Launchpad #2162843): the registration is retried once with `resv[0]=1`.

**Kotlin does the rest** (`src/`): the event loop, the request parser, routing and every handler are the JVM entry's
code, ported (byte arrays instead of `ByteBuffer`, io_uring events instead of selector keys). Connections are slots
in flat arrays (no object per connection). The request head is parsed in one pass: each line is scanned once for its
CR and decoded only if its first character and the colon at the fixed offset of its name say it is Content-Length,
Authorization, Connection or Transfer-Encoding (any case, as before); parse time halved (1.5 → 0.7 % of CPU). JWTs: HS256 with the key's inner/outer pad states hashed once at start
(own SHA-256 in Kotlin); `exp` is required; every token is verified.

**Reads through SQLite's B-tree** (`csrc/btree_reads.c`, allowed by the 2026-10-31 ruling):
- `GET /posts/:id`: the `posts` row by rowid, its `users` row by rowid, and the like count by walking
  `likes_post_id_idx` from the first entry equal to the id. `GET /feed`: the first 20 entries of
  `posts_created_at_id_idx` (`created_at DESC, id DESC`, so the newest come first), then the same for each post.
- The JSON is written exactly as `json_object()` writes it (SQLite's own `jsonIsOk[]` escaping table, same control
  character forms). `KN_VERIFY=1` compares the B-tree path with the SQL path, byte for byte, for every post id and
  the feed, and exits (0 mismatches on the seed: 500,010 ids; also after the test suite's bodies with quotes,
  backslashes, control characters and non-ASCII).
- Anything unexpected (a column type, a spilled record larger than the scratch buffer, a post whose user row is
  missing) returns to the original SQL query for that request. At start the server checks the column order, sort
  order and collation of every table and index it decodes, and serves everything through SQL if they differ.
- Only cursors, `KeyInfo`s, prepared statements and four root page numbers are kept. No rows, no responses.

**One read snapshot across loop passes** (idea: rust-iouring-rkkautsar). The reader connection keeps its read
transaction open between passes and drops it right before it commits a batch (so a request that arrives after a
201 always sees that write) or when the checkpointer waits for readers. Fewer WAL read-lock round trips.

**Writes and durability.** WAL, `synchronous=NORMAL`. Writes are queued and group-committed once the oldest has
waited 20 ms or 1,024 are queued (reads never wait for it); the 201s go to the kernel only after `COMMIT` has
returned. A failed COMMIT answers every write of the batch with a 500.
- **The commit runs on the loop, on the reader's own connection** (default). A COMMIT by *another* connection makes
  the reader drop its whole page cache at its next read transaction (SQLite's WAL-index header check), so with a
  separate committer connection most post reads became a `pread` (finding of the JVM port, kotlin-jvm-uring). The
  loop ends its read snapshot, runs `BEGIN IMMEDIATE` with busy timeout 0 (if a checkpoint RESTART holds the write
  lock, the batch stays staged and every pass end retries while reads go on), the INSERTs, `COMMIT`, and answers at
  once. Measured at 160k users: 14.8-15.2 → 14.0-14.9 µs/req (3 pairs), the committer thread's 0.7 µs/req gone.
- **Slow-disk guard.** A COMMIT on the loop blocks the loop for as long as its WAL `write()` takes (on a busy disk a
  few took ~2 s here once, and every read waited behind them; that is why the commit used to be on a thread). If an
  on-loop commit takes more than 50 ms (`KN_SLOW_MS`), or a checkpoint pass more than 200 ms (`KN_SLOW_CK_MS`: its
  fsync feels a slow disk first), or memory use reaches 90 % (`KN_SLOW_MEM_PCT`; the cgroup's `memory.current` /
  `memory.max` if it has a limit, else `MemAvailable` / `MemTotal`; checked every 50 ms by the checkpointer thread:
  near the wall, reclaim made two on-loop commits take ~2 s at 448k/480k users, the loop stopped draining sockets and
  the server was OOM-killed; with this trigger 448k passes with p99 22 ms), the next commits go to the
  **committer thread** with its own connection (the loop keeps serving reads; the thread wakes it through an
  eventfd on ring 0) for 10 s (`KN_SLOW_HOLD_MS`), extended while commits, checkpoints or memory stay bad and
  doubled when that lasts (up to 5 min); so at most one loop stall per slow spell.
  Tested with an injected 200 ms delay in every WAL write: one 407 ms stall, then 66 commits on the thread while
  `GET /feed` never took more than 50 ms. `KN_COMMITTER=1` = always the thread (the previous design).

**One `pwrite` per commit.** A VFS of our own over the base VFS (public `sqlite3_vfs` API) changes only the WAL
file's writes: SQLite writes each frame as two writes (24-byte header, page); writes that continue where the last one
ended are gathered and go to the file when the commit frame's page arrives (its header's database-size field is
non-zero), inside SQLite's own call, so the commit is in the kernel before SQLite publishes it in the WAL index and
before `COMMIT` returns. Any other write, and every read, sync, truncate, lock, file control and close, writes the
buffer first. Write syscalls 0.50 → 0.145 per request.

**Page cache.** SQLite's own pcache1 through `SQLITE_CONFIG_PCACHE2` with one method of ours, `xRekey`: a B-tree
split swaps two pages through the lock-byte page's number (262,145), after which pcache1's next truncate walked its
whole hash table; here the page waits out the swap under key 0, which SQLite never gives a page.
`pcache1TruncateUnsafe` 0.55 → 0.25 % of CPU. `PRAGMA integrity_check` is ok after every measured run.

**Checkpoints off the loop.** A thread at nice 5 runs a PASSIVE checkpoint every 50 ms on its own connection; once
the WAL has 4,096 frames and all but 128 are copied, a RESTART with a 250 ms busy timeout briefly holds the writer
(the loop's `BEGIN IMMEDIATE` gets SQLITE_BUSY and retries at the next pass end) and the WAL starts over from frame 1.
Idea credit: swift-iouring-akbashev, rust-iouring-kensac.

**SQLite build options**: `SQLITE_THREADSAFE=2`, `SQLITE_ENABLE_QPSG` (plans fixed at prepare time),
`SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_OMIT_DEPRECATED`, `SQLITE_OMIT_SHARED_CACHE`,
`SQLITE_DQS=0`, `SQLITE_USE_ALLOCA`. Pragmas: `mmap_size=0`, `cache_size=-65536` (64 MiB, `KN_CACHE_KB`),
`wal_autocheckpoint=0`. A 256 MiB reader cache (the whole 248 MB seed fits) measured no better at 160k users with the
commit on the reader connection (14.1 vs 14.5 µs/req, 3 rounds), so it stays at 64 MiB (memory is the limit at the
top). Rule 5 allows SQLite's own page cache explicitly, at any size.

**Profile-guided C, trained on the box during the build.** `build.sh` first builds an instrumented server
(`-fprofile-generate`, the Kotlin/Native clang's own profile runtime), writes a synthetic scratch database
(`train/mkdb.c`: the schema, 20,000 users, 50,000 posts, ~200,000 likes from random CTEs, bodies with quotes,
backslashes and non-ASCII; nothing from the seed), serves it on `127.0.0.1:KN_TRAIN_PORT` (3999) to a training client
(`train/train.c`, 64 keep-alive connections running the load test's user loop without think time plus the test
suite's error cases every 64th loop; HS256 tokens signed with the run's own random secret) for 200,000 requests
(~8 s), stops it with SIGTERM (the instrumented build writes its profile then), merges the profile with the bundled
`llvm-profdata`, and rebuilds the C with `-fprofile-use`. The profile exists only in `build/pgo` on the box; nothing
is committed (LEADERBOARD, Arjay's 2026-11-15 ruling on rule 10: "profile-guided optimization is fine when
`build.sh` records the profile on the box during the build (a training run on a scratch database)"). Time limits:
mkdb 300 s, the server 1,200 s (SIGKILL), the client 600 s, health wait 60 s; any failure prints
`!! PGO training failed` and builds without a profile. Measured: feed plan 6.1-6.2 → 4.9-5.1 µs per call (`KN_VERIFY`
timing, 4 of 5 pairs), post plan −4 %; under load user CPU 5.0 → 4.3-4.5 µs/req. `KN_CPGO=0` = plain build.
`-O3 -march=native` on top measured the same as `-O2` with the profile, so the default stays `-O2` (`KN_CFLAGS`).


## Measured (LOCAL, relative only)

i7-10700K desktop shared with other jobs, the server pinned to one core in a 2 GB / no-swap cgroup with
`ulimit -n 65535`, inside a private network namespace (no host conntrack); kload (a k6-compatible replay of
`bench/load.js`) in its own cgroup on other cores. A desktop core is faster than the droplet's vCPU, and loopback is
not a NIC, so these numbers rank designs, they are not a score. Run-to-run noise on this machine is about ±10 %.

| Users | Run | Result | CPU µs/req | CPU | p95 / p99 | RSS | cgroup peak |
|---|---|---|---|---|---|---|---|
| 160,000 | 60 s | PASS | 13.4 (A/B rounds: 12.5-13.3; b3 the same day 14.8-15.2; the JVM entry 41-45) | 23 % | 8.3 / 20.4 ms | 73 MB | 703 MB |
| 256,000 | 60 s | PASS | 12.2 (b3: 14.0-16.0 the same day) | 33 % | 8.6 / 19.6 ms | 85 MB | 1,083 MB |
| 416,000 | 5-min hold | PASS, 0 errors | 12.9 (b3: 14.3) | 57 % | 10.8 / 21.9 ms (reads p99 7 ms, writes 27 ms) | 133 MB | 1,816 MB |
| 448,000 | 60 s | PASS | 12.1 | 58 % | 10.9 / 22.1 ms | 117 MB | 1,851 MB |
| 480,000 | 60 s | PASS once, FAIL once (p99 946 / 1,106 ms) | 12.4-12.9 | 63-65 % | at the 2 GB wall | 118 MB | 1,997-2,002 MB |

- Where it goes (perf, 160k, tmpfs): kernel 8.5 µs/req (TCP send/recv and loopback delivery 7.1 of it), user
  4.4 (B-tree reads + JSON ~2, Kotlin ~0.7). java-iouring-davidhsv on the same rig: kernel 8.8, user 5.6.
- Earlier steps (160k users): SQL only 30-44 µs/req → B-tree reads 22-30 → b3's loop/commit tricks ~15 → this round
  (commit on the reader, PGO) ~13.
- On this desktop the limit is memory, not CPU: ~4.1 KB of kernel socket memory per connection; 480k users reach
  the 2 GB wall. Process RSS is 20 MB idle and ~70-130 MB under load. A real 1 vCPU box stops much earlier (below).
- Build: from a clean `build/` (SQLite and liburing downloaded again; the Kotlin/Native toolchain already in
  `~/.konan`) on one core inside a 1.5 GB no-swap cgroup: 178 s including the ~8 s training run, cgroup peak 584 MB
  (548 MB anonymous). The first build on a box also downloads the Kotlin/Native toolchain (~560 MB anonymous peak).

## Measured on a twin of the scored box (VM, not a score)

Vultr `vhp-1c-2gb-intel` (Cascade Lake, 1 vCPU, 1,961 MiB, Ubuntu 24.04, kernel 6.8.0-117 as on the scored box, no
swap, no ufw, no sysctl changed), load from four 4-vCPU VMs in the same datacenter, one source IP each, over a
private network. Each run on a fresh start and a fresh seed copy; `load.js`'s user model and thresholds (kload,
histograms merged across the four generators), 60 s ramp.

- **Calibration.** With the VM's stock single NIC queue the host drops packets above ~30k packets/s. Every entry
  then collapses at ~160-176k with mass retransmit timeouts, which the scorer never shows. With 4 NIC queues
  (`ethtool -L … combined 4`) that loss is gone, and java-iouring-davidhsv fails at the top the way it fails on the
  scorer (on p99, 0 errors). Its ceiling is still lower on this VM: a 176,000 hold (p99 913 ms) against 240,000
  officially (p99 961 ms). The JVM entry kotlin-nio-3x3xX3N0N costs 91 µs/req here against its official 79. So
  compare the two entries with each other, not the users with the leaderboard.

| Users | Run | This entry | java-iouring-davidhsv |
|---|---|---|---|
| 160,000 | 60 s step | PASS, p99 363 ms, 42.9 µs/req box | PASS, p99 444 ms, 42.9 |
| 176,000 | 60 s step | PASS, p99 346 ms, 42.5 | PASS, p99 791 ms, 43.2 |
| 176,000 | 5-min hold | **PASS**, 0 errors, p95 / p99 42 / 281 ms, 41.6 µs/req box (36.0 process), RSS 146 MB | **PASS**, 0 errors, 71 / 913 ms, 42.5 (35.6), 343 MB |
| 184,000 | 60 s step | FAIL on p99 (1.16 s), 0 errors | FAIL on p99 (1.43 s), 0 errors |
| 192,000 | 60 s step | FAIL on p99 (2.7 s), 0 errors | FAIL on p99 (3.1 s), 0 errors |

- On this VM the two cost the same per request box-wide. This entry's tail latency is lower at every load.
- Before the buffer change (1,024 provided buffers per ring, at most 256 re-arms per pass), this entry failed the
  160,000 step on p99 1.2 s and passed 144,000 (step and 5-min hold, p99 100 ms). A burst of `ENOBUFS` at the end
  of the ramp and at the start of the ramp-down left thousands of receives waiting for seconds.
- The slow-disk guard fired once in the 176,000 hold (a 57 ms commit as the ramp-down began; 441 commits went to the
  thread for 10 s, then back to the loop) and never in the 60 s steps.
- Build on the box (1 vCPU): `install.sh` 5-17 s, `build.sh` 203-240 s including the PGO training run,
  `test/run.sh` 42/42.

## Knobs (all optional; the defaults are what is scored)

`KN_VFS` (base SQLite VFS, default `unix`), `KN_WALVFS=0` (stock WAL writes), `KN_PCACHE=0` (stock pcache1),
`KN_BTREE=0` (SQL only), `KN_SNAPSHOT=0` (new read transaction every pass), `KN_WINDOW_US` / `KN_BATCH_MAX` (group
commit, default 20000 / 1024), `KN_PACE_NR` / `KN_PACE_US` (batch wake, default 256 / 1000; `KN_PACE_NR=1` = off),
`KN_DEFER=1` (`DEFER_TASKRUN` rings), `KN_PASS=sweep` (the davidhsv pass structure; `KN_PACE_ALWAYS`), `KN_COMMITTER=1`
(commit on the committer thread always), `KN_SLOW_MS` / `KN_SLOW_CK_MS` / `KN_SLOW_MEM_PCT` / `KN_SLOW_HOLD_MS` (slow-disk
guard, default 50 / 200 / 90 / 10000; `KN_SLOW_MEM_PCT=0` = no memory trigger), `KN_CACHE_KB` (reader page cache, default 65536), `KN_MAX_EV` (events per pass, default 4096), `KN_LISTENERS` (1-8),
`KN_HDR=0` (the old two-pass head parser), `KN_MLOCK=0`, `KN_RINGS`, `KN_NBUF` (provided receive buffers per ring, a power
of two, default 4096), `KN_REARM` (receives re-armed per pass after `ENOBUFS`, default 0 = all), `KN_STATS=1` /
`KN_PROF=1` / `KN_CKPT_LOG=1` (counters on stdout/stderr every 10 s; `KN_STATS` also counts why connections ended, by
errno), `KN_STALLS=1` (log passes over 100 ms), `KN_VERIFY=1` (the byte-for-byte
check above). Build: `KN_CPGO=0` (no profile), `KN_CFLAGS`, `KN_TRAIN_PORT` / `KN_TRAIN_REQS` / `KN_TRAIN_CONNS` /
`KN_TRAIN_TIMEOUT`. Test only: `KN_TEST_WAL_DELAY_US` (sleep in every WAL write, for the slow-disk guard).

Extra checks besides `test/run.sh`: `test-restart.sh` (a restart right after a stop with open connections comes up
healthy; on Ubuntu's 6.8 kernel the old process's io_uring keeps its listener alive briefly after exit, so `bind()`
is retried for up to 30 s) and `test-rings.sh` (with tiny tables, 2,500 connections spill over 8 rings and all are
answered).

## Credits (ideas only; no files or code copied; all MIT)

- rust-iouring-kensac: rings spilling past the fd cap, 512 accepts in flight, soft `RLIMIT_NOFILE` raised to hard,
  re-arm after `ENOBUFS` (bounded per pass there; the `KN_REARM` knob here), the buffer-ring registration quirk,
  B-tree cursor reads with schema checks at start.
- c-iouring-pavlyhalim: provided-buffer receive, B-tree read helpers as a pattern.
- kotlin-trikeshed-jnorthrup: Kotlin/Native + SQLite amalgamation built on the box; the head parsed as a struct
  (first character, then verify at fixed offsets); batch wake (256 completions or 1 ms).
- fsharp-uring-delneg: the pcache1 `xRekey` method; one `pwrite` per WAL commit; `mlock` of the binary.
- c-iouring-pavlyhalim: the pcache1 truncate diagnosis, the WAL write-coalescing VFS, `mlock` of the binary.
- go-iouring-DblD: a wider write window (reads never wait for it).
- rust-iouring-rkkautsar: one read snapshot across passes.
- swift-iouring-akbashev: checkpointer at nice 5 with a bounded hold.
- zig-iouring-unorig-username (via the JVM entry): the two-step feed idea that led to the B-tree feed.
- java-iouring-davidhsv: the sweep pass structure (`KN_PASS=sweep`: sweep the other rings without waiting, then
  wait on ring 0 alone; the zero-timeout second enter) and the on-loop commit on the reading connection.
- c-iouring-pavlyhalim: PGO trained during the build on its own synthetic database (the approach; our mkdb/train
  are our own, from the Graal entry kotlin-graal-3x3xX3N0N).
- Our own JVM port (kotlin-jvm-uring-3x3xX3N0N): the page-cache-reset finding behind committing on the reader.

## Licences of what the build fetches

SQLite: public domain. liburing: MIT (dual MIT / LGPL-2.1; used under MIT). Kotlin/Native compiler and runtime:
Apache-2.0 (its clang/LLVM bundle: Apache-2.0 with LLVM exceptions; glibc sysroot: LGPL). Temurin JDK: GPLv2 with the
Classpath Exception (build time only).
