# WAT entry worklog (opt-box: i5-11400F VM, 4 vCPU, x86_64; NOT the grader)

Harness: `ub.sh` starts the server pinned to CPU 0 on a fresh copy of the seed (on disk, `~/watlab/wb`),
then `ul` (own C load generator, `bench/load.js` user loop with think times x SCALE, keep-alive,
reconnects on `Connection: close`) on CPUs 1-3. us/req = server process CPU (utime+stime) / requests
in the measure window. Loopback: part of the server's kernel TCP work is charged to the client cores.

## Setup (1 Nov)
- opt-box `~/watlab`: wasi-sdk 34 (clang 23.1.0), wabt 1.0.39, binaryen 125, WasmEdge 0.18.1,
  SQLite 3.54.1 amalgamation (sha256 0a9c8e34… = the ObjC entry's pin). Seed copied from `~/tdsc/repo/seed`.
- Reference `wserve.c` (C, poll loop) native: 42/42; WasmEdge AOT: 42/42.
- WasmEdge 0.18.1 source read (`lib/host/wasi/inode-linux.cpp`): `poll_oneoff` keeps one epoll fd but
  does an `epoll_ctl` ADD/MOD for every subscription on every call, DELs the ones not passed again,
  and an `ioctl(FIONREAD)` per ready fd. So a call costs O(subscriptions) syscalls. WASI fds are random
  32-bit numbers (an `unordered_map`), so the program cannot index by fd; poll userdata carries the slot.
- WasmEdge's listener needs `sock_setsockopt(SO_REUSEADDR)` or a restart fails to bind while old
  connections sit in TIME_WAIT ("wasmedge socket setup failed").

## E1: keep-alive vs a new connection per request (Connection: close), C reference
4000 users, scale 0.1 (~4.2k req/s), 15 s, 3 interleaved reps.
| variant | us/req (3 reps) | p99 ms |
|---|---|---|
| native, keep-alive (poll over ~4000 fds) | 189.3, 188.4, 189.0 | 34-37 |
| native, Connection: close | 17.48, 17.49, … | 34-38 |
| WasmEdge AOT, keep-alive (epoll_ctl per fd per call) | 134.5, 137.7, … | 38-42 |
| WasmEdge AOT, Connection: close | 20.19, 19.51, … | 33-36 |
Finding: with every user holding an idle keep-alive connection, the per-call O(n) poll dominates
(7-10x). Closing after every response keeps the poll set tiny. load.js think times are >= 3 s, so
keeping a connection for less than 3 s never gets reused anyway, and keeping it longer at 280k users
would need ~80k+ fds (LimitNOFILE is 65,535). Decision: answer with `Connection: close`.
p99 ~35 ms in every variant: probably the inline WAL checkpoint (to check).

## Linking test (1 Nov)
`wat2wasm main.wat` + SQLite reactor module (wasi-sdk clang, `-mexec-model=reactor`, `--global-base=16 MiB`)
joined by binaryen `wasm-merge sq.wasm sq main.wasm main`: the WAT module imports the SQLite module's
memory and functions; runs under WasmEdge interpreter and AOT. wasm-ld default is stack-first
(stack [0, 64 KiB)); with `-z stack-size=1 MiB` and `--global-base=16 MiB` the WAT owns [1 MiB, 16 MiB).

## SQLite survey (user request, 1 Nov)
How the top entries use SQLite (grep of their sources):
- All: WAL + `synchronous=NORMAL`, one connection, persistent prepared statements, group commit per batch,
  `temp_store=MEMORY`, `unix-excl` VFS (WAL index on the heap, no shm), mostly `mmap_size=0`
  (ObjC measured a 32 MB page cache ~3% faster than mmap), cache_size -512 KiB … -64 MiB.
- Checkpoints off the event loop: `wal_autocheckpoint=0` and PASSIVE checkpoints from a second
  connection on a thread (Lean, Mojo, pavlyhalim, public, shivanshthapliyal, V) or a forked child (ObjC),
  with a write gate around 8-16k frames. c-iouring-public's inline "bounded" mode held 17.5k vs 15.5k req/s
  depending on pass size: inline checkpoints stall the loop.
- Compile flags (Mojo/ObjC/public/pavlyhalim share most): THREADSAFE=0/2, DEFAULT_MEMSTATUS=0,
  DEFAULT_WAL_SYNCHRONOUS=1, DQS=0, LIKE_DOESNT_MATCH_BLOBS, MAX_EXPR_DEPTH=0, OMIT_DEPRECATED,
  OMIT_SHARED_CACHE, OMIT_PROGRESS_CALLBACK, OMIT_LOAD_EXTENSION, USE_ALLOCA, OMIT_DECLTYPE,
  (pavlyhalim also OMIT_AUTOINIT/JSON/UTF16/TRACE, TEMP_STORE=3, own PCACHE2 plugin).
- Fastest reads: SQLite B-tree cursor calls from a TU that includes the amalgamation (ObjC, Mojo,
  pavlyhalim, public, V).

Existing wasm SQLite builds, checked for WASI use:
- SQLite official `ext/wasm`, sql.js, wa-sqlite: Emscripten, browser VFS (OPFS/IndexedDB). Not WASI.
- ncruces/go-sqlite3 (build now in ncruces/go-sqlite3-wasm `build/build.sh`): freestanding `--target=wasm32`,
  own libc, `SQLITE_OS_OTHER` with the VFS in Go on the host, `wasm-opt -O4 --gufa-optimizing …`, then
  wasm2go. Not WASI (its VFS is host code). Reusable: its option list (`sqlite_opt.h`) and the wasm-opt pass.
- VMware Wasm Labs (webassembly-language-runtimes) libsqlite: their patches were upstreamed;
  since 3.41.0 stock SQLite builds for WASI (`SQLITE_WASI`: no fchmod/mremap, default VFS
  `unix-dotfile`, no shm). Their article says WAL is off in their build; with
  `locking_mode=EXCLUSIVE` WAL works with the index on the heap (the reference 42/42 runs that way).
Decision: build the stock pinned amalgamation with wasi-sdk (that is the WASI-ready path), borrow
flags from the top entries / ncruces, and A/B: flag set, wasm-opt on the SQLite module, page cache,
VFS (`unix-none` avoids a stale `.lock` dir after a crash), checkpoint policy.
Open: checkpoints. No fork/threads in plain WASI; WasmEdge wasi-threads support to check.

## E2: the WAT server, first build (1 Nov) — 42/42
Built by `experiments/dev-build.sh` on opt-box: SQLite 3.54.1 + `src/sqlite_glue.c` (wasi-sdk 34, `-O2`,
reactor, 1 MiB stack, data from 16 MiB) and `src/app.wat` (wat2wasm 1.0.39), joined by `wasm-merge`
(binaryen 125), `wasmedge compile --optimize=3` → `app.so` (3.7 MB), run by `src/host.c` (WasmEdge 0.18.1
C API, `WasmEdge_ConfigureSetRunMode(AOT)` is required or the .so is rejected).
Design: SPEC wants HTTP/1.1 keep-alive, so instead of E1's `Connection: close` the host gives the WAT
8 raw Linux calls ("sys": listen, epoll_create/ctl/wait, accept4, recv, send, close); level-triggered
epoll, per-fd state in a table indexed by the real fd. WASI is kept only for SQLite's file I/O, env and
clock. SQLite: one connection, `unix-none` VFS, locking_mode=EXCLUSIVE, WAL, synchronous=NORMAL,
64 MiB cache, persistent prepared statements, default autocheckpoint (inline).
Result: `t.sh e2 out/host out/app.so` → passed=42 failed=0 on the first full run.

## E3: WAT (epoll, keep-alive) vs the E1 references (1 Nov)
4000 users, scale 0.1 (~4.2k req/s), 15 s, 3 interleaved reps, server on CPU 0.
| variant | us/req (3 reps) | p99 ms |
|---|---|---|
| WAT, WasmEdge AOT, epoll host, keep-alive | 9.91, 10.09, 13.39 | 32-35 |
| C native, poll, Connection: close | 17.34, 19.04, 16.87 | 34-36 |
| C→wasm WasmEdge AOT, poll_oneoff, Connection: close | 19.17, 19.36, 18.98 | 35-38 |
| C native, poll, keep-alive | 186.7, 182.2, 185.7 | 32-36 |
The epoll host plus keep-alive takes the WAT build below the best E1 variant at this load. Not yet a fair
WAT-vs-C test: the C side still uses poll. E4 puts the same C on the same epoll loop and host.
p99 ~33 ms everywhere: still suspect the inline WAL checkpoint.

## E4: same epoll loop and host, WAT vs C→wasm vs C native (1 Nov)
`baseline/cserve.c` = the C reference (wserve.c) with its poll loop replaced by the WAT entry's loop and
"sys" calls; wasm build runs on the same `host` (42/42), native build uses the calls as syscalls (42/42).
4000 users, scale 0.02 (~20k req/s), 20 s, 3 interleaved reps.
| variant | us/req | p95 ms | p99 ms | rss |
|---|---|---|---|---|
| WAT, WasmEdge AOT | 7.91, 7.84, 7.92 | 38.2, 34.7, 38.0 | 44.3, 40.7, 135.2 | 59 MB |
| C→wasm, WasmEdge AOT, same host | 7.54, 7.99, 7.52 | 35.3, 35.9, 35.3 | 39.5, 40.4, 41.7 | 96 MB |
| C native (gcc -O2), epoll | 8.41, 8.60, 8.82 | 33.0, 32.5, 32.7 | 38.7, 37.8, 38.0 | 48 MB |
Server CPU is only ~16% at 20k req/s: loopback charges much of the TCP work to the client cores, and
most of a request's server cost is SQLite. WAT ≈ C→wasm here (C→wasm ~2% lower median, within noise);
both wasm builds are a bit below native (unexplained; malloc? check later). The real problem: p95
~35 ms in all three at 20k req/s. Suspect the inline WAL autocheckpoint (E5).

## E5: inline autocheckpoint off (diagnostic only; WAL grows without bound) (1 Nov)
Same load as E4 (4000 users, x0.02, 20 s), 3 interleaved reps.
| variant | rps | us/req | p95 ms | p99 ms |
|---|---|---|---|---|
| WAT (autocheckpoint 1000) | 20215, 20285, 20357 | 7.54, 8.06, 7.79 | 38.4, 36.8, 35.9 | 44.6, 42.3, 40.9 |
| WAT, wal_autocheckpoint=0 | 21134, 21192, 21187 | 8.16, 7.90, 7.91 | 0.40, 0.35, 0.35 | 4.45, 0.60, 0.55 |
Confirmed: the ~35 ms p95 is the inline checkpoint (its fsyncs block the loop); CPU per request is
about the same, so the cost is blocked time, not CPU (~4.5% fewer requests served in the window).
Not kept (can't ship without checkpoints). Next: checkpoints off the loop (no threads/fork in WASI).

## Setup: install.sh / build.sh / start.sh + clean-container CI mimic (1 Nov)
install.sh (root): apt build-essential/curl/unzip, then wabt 1.0.39, wasi-sdk 34, binaryen 125 and
WasmEdge 0.18.1 (ubuntu20.04 build) release tarballs, each sha256-pinned, into /opt/wat-tdsc (the same
way merged entries fetch LLVM/Lean/Mojo toolchains). build.sh (user): SQLite 3.54.1 amalgamation
(sha256 0a9c8e34…) → sq.wasm, app.wat → app.wasm, wasm-merge, `wasmedge compile`, gcc host.c.
CI mimic (`~/watlab/ci.sh`: fresh ubuntu:24.04 container, install.sh as root, test/run.sh as a user):
passed=42 failed=0.

## E6: past the fd cap — idle keep-alive eviction (1 Nov)
Harness: `ul` now binds source addresses 127.0.0.2-9 (more than 64k client ports) and reports error kinds;
`ub.sh` limits only the server to 65,535 fds (like the grader's LimitNOFILE). Real think times (scale 1.0),
ramp 30 s, measure 30 s, single runs (a cliff test, not an A/B).
| variant | users | rps | us/req | p95/p99 ms | err | note |
|---|---|---|---|---|---|---|
| E2 build | 40,000 | 4235 | 10.78 | 4.2 / 37.3 | 0 | 80k sockets, ~600 MB RAM |
| E2 build | 80,000 | 7025 | 55.28 | 20.9 / 39.6 | 0* | ~14k users never accepted; EMFILE busy-loop on the level-triggered listener (*ul has no timeout; k6 would time out) |
| evict ≥1.5 s idle when >60k conns | 80,000 | 8435 | 15.02 | 35.4 / 125.0 | 0.77% | ECONNRESET: closed sockets that already held an unread request |
| + read once before closing | 80,000 | 8417 | 14.69 | 40.2 / 171.4 | 0.000% | recv0=141 over the whole run incl. ramp |
Kept: when more than 60,000 sockets are open (or accept hits EMFILE), every 0.5 s close keep-alive
sockets idle ≥ 1.5 s with no partial request or unsent output, after one recv() that must return EAGAIN
(else the request is served). On EMFILE with nothing to free, the listener is paused until the next sweep.
Cost: past ~60k users most requests come on a fresh connection (~15 vs ~11 us/req on loopback; the
grader's real NIC adds more). io_uring direct descriptors (what ObjC/Lean/Mojo use) would lift the cap;
consider later via the host.

## E7: baselines, ObjC #154 and c-iouring-public #158 vs WAT and C→wasm (1 Nov)
The two entries were built by their own install.sh/build.sh in fresh ubuntu:24.04 containers
(`~/watlab/mkbase.sh`), then their `/app` was copied out and run natively by `ub.sh` like ours (CPU 0,
nofile 65,535). In Docker, the default seccomp profile refuses io_uring (ObjC exits, the C entry falls back
to epoll), so native runs are the fair setup. Both use io_uring here (checked in their startup logs).
4000 users, scale 0.02, 20 s, 3 interleaved reps (WAT, C→wasm, ObjC, C), the same load as E4.
| variant | us/req | rps | p95 ms | p99 ms | rss |
|---|---|---|---|---|---|
| WAT, WasmEdge AOT | 8.07, 8.04, 7.99 | 20.1–20.3k | 41.9, 35.4, 35.1 | 48.6, 41.3, 40.8 | 59 MB |
| C→wasm (cserve.c), same host | 7.80, 7.79, 8.25 | 20.3k | 35.4, 35.0, 35.4 | 40.3, 40.2, 40.7 | 96 MB |
| ObjC #154 (objc-uring-delneg) | 6.23, 6.13, 5.95 | 21.1k | 1.5, 1.5, 1.5 | 1.8, 1.6, 1.6 | 25 MB |
| C #158 (c-iouring-public) | 7.22, 6.30, 6.50 | 21.1k | 1.3, 1.25, 1.25 | 1.75, 1.45, 1.5 | 13 MB |
WAT is ~30% more CPU per request than ObjC and ~20% more than the top C entry, and about level with
C→wasm. The wasm builds also serve ~4% fewer req/s at the same offered load, because the inline
checkpoint stalls (p95 ~35 ms) delay each user's next think period. Both baselines checkpoint off the
loop (ObjC: a forked child; C: a thread). They also read /feed and posts through SQLite's B-tree layer
with no bytecode, batch writes into one transaction, and (ObjC) use PGO.

## E8: profile, WAT vs ObjC (perf, 4000 users x0.02) (1 Nov)
`perf record` on the server under E7's load. The AOT `.so` names functions `f<index>`; a copy built with
`wat2wasm --debug-names` and `wasm-merge -g` maps them (`out/prof/names.txt`).
WAT: app.so 57.6% (sqlite3VdbeExec 9.3, BtreeTableMoveto 6.6, BtreeIndexMoveto 4.3,
vdbeRecordCompareInt 3.9, pcache1Fetch 2.9, getAndInitPage 2.6 ... the WAT's own functions together ~6%:
putjson 2.9, post_json 1.9, process 0.6), kernel 33.3%, libwasmedge 6.1% + libc rwlock 1% (host-call and
WASI dispatch). ObjC: kernel 67%, its own code 30% (`_i_TDStore__feed_` 9.9%: the feed on the B-tree layer).
In samples, WAT's user time is ~3x ObjC's while kernel time is about equal. So the gap is SQLite's
bytecode path for /feed and posts, not the WAT code and not the runtime: E9 (group commit) and E10
(B-tree reads) next.

## E9: group commit, one transaction per epoll batch (1 Nov) — kept
BEGIN after each epoll_wait, COMMIT after the batch; responses wait in OUTB (now 12 MiB at 0x400000,
with a record list at 0x300000) and are sent only after COMMIT, so no 201 goes out before its row is
committed; if COMMIT fails, the batch's connections are closed unanswered. Bug found on the way: inside
an explicit transaction, `INSERT ... RETURNING` needs a temp file, and WASI has no temp directory
("disk I/O error", then "cannot commit - no transaction is active"): fixed with `PRAGMA temp_store=MEMORY`.
42/42. 4000 users x0.02, 20 s, 3 interleaved reps:
| variant | us/req | p95 ms | rps |
|---|---|---|---|
| base (c01300f) | 8.03, 7.87, 7.84 | 37.5, 35.2, 34.5 | 20.3k |
| E9 group commit | 7.12, 6.05, 6.38 | 39.1, 37.0, 38.0 | 20.5k |
Median -19% CPU per request; every E9 rep is below every base rep. p95 unchanged (checkpoint stalls).

## Metric correction: ul's CPU number undercounts ~2x (1 Nov)
ul's `cpu=` (and every us/req above, E3-E9) comes from the server's /proc utime+stime ticks over the
window. Under `perf stat -e task-clock` on the same runs it reads about half: e.g. E9 "6-7 us/req" is
21 us/req of task-clock. The comparisons inside one table still hold (same bias for every variant, all
interleaved), but the absolute numbers are low. From here on: `pstat.sh` = ub.sh with a 25 s window and
`sudo perf stat -p <server> -- sleep 15` from second 14; it prints task-clock us/req and user/kernel
cycles and instructions per request. 4000 users x0.013 (~30k req/s, below ul's ~62k ceiling, so every
variant serves the same offered load).

## E10: /feed and GET /posts/:id on SQLite's B-tree layer (1 Nov) — kept
`sqlite_glue.c` now `#include`s the amalgamation (one translation unit) and exports one-line wrappers
over the cursor functions (`sqlite3BtreeCursor`, First/Next/Previous, TableMoveto, IndexMoveto with a
one-field UnpackedRecord, PayloadFetch). The WAT reads the schema's root pages at start, keeps four
cursors (posts, users, posts_created_at_id_idx, likes_post_id_idx) open per transaction, decodes records
itself (varints, serial types) and counts likes by an index range scan (one scan over the 20 feed ids
when their span is < 256, else one seek each). Guards: UTF-8 database, schema cookie checked per
transaction, overflow pages or anything unexpected fall back to the SQL statements. Same pager, page
cache, WAL and locks: only the bytecode engine is skipped. Responses byte-identical to E9 over 314
requests; 42/42. pstat, 4000 users x0.013, 3 interleaved reps:
| variant | task-clock us/req | user cycles/req | user instr/req | kernel instr/req | p95 ms | rps |
|---|---|---|---|---|---|---|
| E9 (fddfada) | 21.03, 21.23, 21.31 | 55.5k, 56.0k, 56.2k | 155k | 28k | 42.5-43.6 | 30.0k |
| E10 | 15.26, 15.25, 15.55 | 28.8k, 28.9k, 29.6k | 76.4k | 28k | 39.3-39.9 | 30.5k |
| ObjC (objc-uring-delneg) | 10.08, 9.47, 9.94 | 10.7k-11.1k | 27.1k | 25k | 1.8-1.85 | 32.4k |
| C (c-iouring-public) | 9.58, 9.48, 9.65 | 10.5k-10.9k | 22.4k | 26k | 1.45 | 32.5k |
-28% CPU per request, user instructions halved. WAT is now ~1.6x the baselines' CPU: user cycles ~2.7x
theirs, kernel about equal. Their p95 is 1.5-1.9 ms against WAT's ~40 ms: the inline WAL checkpoint
(both baselines checkpoint off the loop). E11 next.

## E11: smaller WAL checkpoints (`wal_autocheckpoint` 1000 -> 250 / 100) (1 Nov) — discarded
pstat, 4000 users x0.013, 3 interleaved reps:
| variant | task-clock us/req | p50 ms | p95 ms | rps |
|---|---|---|---|---|
| E10 (1000 pages) | 15.41, 15.33, 15.39 | 0.30 | 38.9-41.1 | 30.4k |
| 250 pages | 15.78, 14.98, 15.17 | 8.5-8.95 | 28.5-28.7 | 30.0k |
| 100 pages | 15.15, 14.98, 14.83 | 22.7-23.3 | 39.3-40.1 | 27.4k |
Shorter stalls but many more, and the median goes from 0.3 to 9-23 ms: each checkpoint costs two
fsyncs whatever its size. opt-box's disk: fsync of 200 KB takes 16.9 ms (p50, 30 tries). Counting
syscalls over 15 s (`fsc.sh`, perf tracepoints): E10 makes 240 fsyncs (8 checkpoints/s, ~3.3k
commits/s), so the loop sits in fsync ~27% of the time; even at ~half CPU this costs 7% of the offered
load (30.5k vs 32.6k served). A diagnostic build with `synchronous=OFF` (not allowed by rule 6, never
kept) has p95 0.40 ms and serves all 32.6k. So: fewer checkpoints (E12) or checkpoints off the loop.

## E12: bigger WAL checkpoints (`wal_autocheckpoint` 4000 / 16000) (1 Nov) — discarded
`fsc.sh` (fsync count over 15 s), 4000 users x0.013, 3 interleaved reps:
| variant | fsyncs / 15 s | p50 ms | p95 ms | p99 ms | max ms | rps |
|---|---|---|---|---|---|---|
| E10 (1000) | 238, 243, 240 | 0.25 | 38.7-40.1 | 45-47 | 54-72 | 30.4k |
| 4000 | 71, 72, 71 | 0.20 | 69.6-73.9 | 83-88 | 99-143 | 30.2k |
| 16000 | 18, 18, 18 | 0.15 | 43.5-73.6 | 189-198 | 219-622 | 30.7k |
Fewer checkpoints, but each copies and fsyncs more distinct pages: ~80 ms at 4000, ~200 ms at 16000.
Blocked time drops (~27% -> ~19% -> ~12%) but the tail gets worse. Consulted DeepSeek and Gemini on
moving fsync off the loop (a VFS shim whose xSync hands the fd to a host thread): both say it is
`synchronous=OFF` in effect (SQLite's noSync path skips exactly those syncs), and the stall would move
to the next WAL restart anyway. Not doing it. A real off-loop checkpointer needs a shared wal-index
(wasi-threads, or a host-backed xShm + a native checkpoint thread): big, and C. Cheaper first: write
less WAL (E13).

## E13: group-commit window of 2 ms; per-fd generation for held responses (1 Nov) — kept
A transaction that has written stays open across epoll_wait calls (timeout = what is left of the
window) until 2 ms after it began; one with no writes commits at the end of its batch as before, so
read-only traffic waits for nothing. Responses are still sent only after COMMIT. Fixes a latent bug the
longer hold would have made likelier: a held response checked only that its fd was open, so a
connection closed and its fd reused by an accept in the same batch could get the old response. Now
each fd has a generation (GEN at 0x144000, bumped on close) and the record keeps it. 42/42.
4000 users x0.013, 3 interleaved reps each of fsc.sh then pstat.sh:
| variant | fsyncs / 15 s | task-clock us/req | user / kernel cycles | p50 ms | p95 ms | rps |
|---|---|---|---|---|---|---|
| E10 | 237, 240, 243 | 15.43, 15.23, 15.34 | 29.1k / 26.0k | 0.25-0.30 | 37.8-43.9 | 30.3k |
| E13, 2 ms | 178, 180, 180 | 14.46, 14.33, 14.37 | 27.8k / 24.1k | 2.35-2.5 | 37.3-43.1 | 30.7k |
| 5 ms | 158, 156, 156 | 15.06, 14.60, 14.31 | 27.5k / 24.2k | 4.7-5.2 | 43.2-47.4 | 29.9k |
-6% CPU per request (fewer commits: less SQLite commit work and fewer WAL writes and syscalls) and
-25% fsyncs. The WAL volume follows the number of writes more than the number of commits: a like
dirties random leaves of two indexes. 5 ms: no better CPU, and the closed-loop users get fewer
requests in. The median latency cost (2.5 ms) is nothing next to the 500 ms p95 budget.

## E14: SIMD fast path in the JSON string writer; fixed lengths for constant strings (1 Nov) — kept
Profile of E13 (perf record, names from a `--debug-names` / `wasm-merge -g` copy): kernel 46%, app.so
42%, libwasmedge 7.7% + libc 3.7% (host-call path: rwlock, free). In app.so: sqlite3BtreeTableMoveto
5.9, putjson 5.0, bt_post_out 3.8, pcache1Fetch 2.6, record 2.5, sha_block only 0.6. `putjson` escaped
byte by byte; `$puts` ran strlen over each constant. Now putjson copies 16 bytes at a time with one
v128 compare while none is '"', '\\' or < 0x20 (then byte by byte as before), and the feed's field
names and the Content-Type header are written with their lengths. Responses identical to E13
(`cmp.py`: /feed + 182 GET /posts/:id hashed, plus 7 POSTed bodies with quotes, backslashes, control
characters and UTF-8 read back through /posts/:id and /feed). 42/42. pstat, 4000 x0.013, 3 reps:
| variant | task-clock us/req | user cycles | user instr |
|---|---|---|---|
| E13 | 14.60, 14.51, 14.33 | 27.6k-28.2k | 76.2k |
| E14 | 13.43, 13.57, 13.74 | 24.7k-24.9k | 66.3k |
-6% CPU per request, -13% user instructions.

## E15: SQLite with `-DUSE_PREAD` (1 Nov) — kept (neutral on CPU)
Syscalls under load (perf tracepoints, 10 s at ~30.7k req/s): recvfrom 308k, sendto 307k, lseek 124k,
writev 100k, readv 21k, epoll_wait 21k, fsync ~120; 876k in all (2.85 per request). Without
`USE_PREAD` SQLite's unix VFS seeks before every read and write, and WASI turns each into `fd_seek` +
`fd_write`. With it: preadv 21k + pwritev 101k, no lseek, 758k syscalls (-14%). pstat, 3 reps:
E14 13.94, 13.60, 13.59 us/req; E15 14.12, 13.42, 13.50. Within noise; kept as fewer syscalls (it may
matter more on a box where syscalls cost more). Also: ~14.5 requests per epoll_wait at this load, and
~6k WAL frames/s (two pwritev each: frame header and page).

## E16: one host call sends the whole batch (`sys.sendmany`) (1 Nov) — kept
Host calls cost: libwasmedge 7.7% + libc 3.7% (rwlock) of the E13 profile, ~1.5 us per request over
~2.3 host calls. `$flush` made one `send` host call per response; now it marks records whose connection
is gone (fd -1, by generation), hands the record list to `sendmany` (host: one `send` per record, results
into RBUF, up to 32768 per call), then handles partial sends and errors as before. host.c +12 lines, no
logic (a loop over records and one syscall each). Responses identical (cmp.py), 42/42. pstat, 3 reps:
E15 13.58, 13.93, 13.77 us/req; E16 12.99, 13.29, 13.32 (-3.6%; every E16 rep below every E15 rep).

## E17: one host call waits and receives (`sys.epoll_recv`) (1 Nov) — kept
The host's epoll_recv does epoll_wait, then one `recv` (4 KiB) for each event that is neither the
listener's nor write-ready, into RSLOT (0xe00000, 512 x 4 KiB; OUTB shrinks to 10 MiB). The WAT serves a
connection straight from its slot, or joins it after an unfinished request's bytes in RBUF; a request
longer than 4 KiB just takes more batches (epoll is level-triggered). Two guards, because the bytes are
now read before the WAT looks at the connection: every event's connection is marked active before the
eviction sweep can run, and each event keeps its connection's generation so an event whose connection
was closed in between (a send error during an early flush) is skipped. Tests: 42/42, cmp.py identical,
`big.py` (a 12 KB request over several slots, 3 pipelined requests, a sender dribbling 7 bytes every
2 ms) same as E16. pstat, 3 reps: E16 13.45, 13.38, 13.03 us/req; E17 13.08, 12.91, 12.97 (-3%).

## E18: SQLite at clang -O3; binaryen `wasm-opt -O3` before AOT (1 Nov) — wasm-opt kept
pstat, 3 interleaved reps: E17 13.02, 13.09, 13.04 us/req; SQLite -O3 13.31, 12.97, 13.16 (nothing:
WasmEdge's LLVM -O3 sees it all anyway); wasm-opt -O3 on the merged module 12.72, 12.85, 12.90 (-1.5%,
user instructions 63.1k -> 61.7k; every rep below every E17 rep). 42/42 on both. Kept wasm-opt (binaryen
is already installed for wasm-merge).

## S1: saturation under a CPU cap (1 Nov) — measurement, no change
`sat.sh` runs the server in a `systemd-run --user --scope -p CPUQuota=40%` scope and loads it past its
limit (4000 users x 0.007), reading the scope's `cpu.stat` over 15 s. E18: 34.5k, 34.8k, 33.7k rps;
ObjC 49.8k; top C (cpub) 50.0k: WAT is at 0.69x. Per request at the cap: E18 11.55 us (user 5.5, sys 6.1),
ObjC 8.17 us (user 2.5, sys 5.6). The sync=OFF diagnostic build (never kept) gives 34.6k, 35.1k: only
+2%, so at saturation fsync stalls are not the limit; user CPU is (2.2x ObjC's). Profile of E18 at
4000 x 0.013: kernel 51.6%, app.so 39.1%, libwasmedge 6.2%, libc 2.6%; top of app.so:
sqlite3BtreeTableMoveto 5.55, pcache1Fetch 2.71, record 2.68, bt_post_out 2.51, putjson 2.16,
getCellInfo 1.94, getAndInitPage 1.94, IndexMoveto 1.43, PcacheRelease 1.24.

## S2: C→wasm at saturation (1 Nov) — measurement
Same harness as S1, 2 interleaved reps: C→wasm (`baseline/cserve.c`, E4's build) 15.9k, 15.3k rps at
25.2, 26.3 us/req; E18 35.2k, 34.5k at 11.4, 11.6. The C→wasm baseline is the plain C reference (one SQL
statement set, no B-tree reads, no group-commit window, no batched host calls), so this is mostly the
design, not the language: E4 measured WAT ≈ C→wasm with the same loop and the same SQL.

## E19: ObjC's SQLite compile options (1 Nov) — discarded
`-DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK
-DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA`. 42/42. pstat, 3
interleaved reps: E18 12.95, 13.00, 12.84 us/req; E19 13.00, 12.94, 12.68; user instructions the same
(61.7-62.1k). Noise; discarded.

## E20: `$put` without memory.copy (1 Nov) — (a) kept, (b) discarded
A profile (perf record) put ~6% in libwasmedge and ~0.7% in pthread_rwlock; disassembly of the hot
libwasmedge code shows a function that looks up two memory instances, bounds-checks (`pages << 16`) and
calls memmove: WasmEdge's AOT code makes every `memory.copy` a call into the runtime. `$put` (some 200
calls per `/feed`) used one memory.copy per call. (a) `$put` copies with v128/i64/i32/byte loads and
stores, and keeps memory.copy for over 256 bytes. (b) also SQLite: its own memcpy/memmove/memset with
plain loads/stores, built with `-mno-bulk-memory -mno-bulk-memory-opt` (LLVM 23 splits the latter out;
`-mno-bulk-memory` alone still inlines memory.copy). 42/42, cmp.py identical, big.py same, both.
pstat, 3 interleaved reps: E18 13.18, 12.97, 13.06 us/req; E20a 12.67, 12.48, 12.59 (-3.7%; user
instructions 61.9k -> 56.5k, every rep below every E18 rep); E20b 12.65, 12.82, 12.53 (no better than a:
SQLite's copies aren't on the hot read path). Kept (a) only.

## S3: saturation, E20 vs the baselines (1 Nov) — measurement
`sat2.sh`, CPUQuota=40%, 4000 users x0.007, 3 interleaved reps (rps; scope CPU per request):
E20 35.3k, 35.8k, 35.8k (11.33, 11.19 us); ObjC 49.5k, 49.7k, 48.7k (8.08, 8.06 us); top C (cpub)
50.4k, 49.9k, 48.5k (7.94, 8.02 us). WAT at 0.72x of either; p95 at saturation ~105 ms against ~45 ms.
