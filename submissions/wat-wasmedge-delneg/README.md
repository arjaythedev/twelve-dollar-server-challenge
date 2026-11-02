# WebAssembly, hand-written (WAT) / WasmEdge AOT / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx). The server is written by hand in WebAssembly's text format,
compiled ahead of time to native code by WasmEdge, and run by a small C embedder.

## What is WAT and what is C

| File | Language | What it does |
|---|---|---|
| `src/app.wat` | WAT, hand-written | Everything a request touches: the io_uring event loop (it writes every submission entry and reads every completion itself), HTTP/1.1 parsing, routing, JSON reading and writing, base64url, SHA-256 and HMAC for the HS256 JWT check, SQL statement calls, group commit, when to checkpoint the WAL and the io_uring fsync of the database file, and the B-tree read paths (record decoding, which cursors to open and seek) |
| `src/host.c` | C, ~200 lines | WasmEdge embedder: loads the AOT-compiled module, gives it WASI (used only by SQLite for file I/O, env vars and the clock) and six Linux imports: `listen`; `ring_new` (sets up an io_uring and its fixed-file table); `ring_pbuf` (registers a provided-buffer ring that lives in wasm memory); `ring_go` (copies the SQEs the WAT wrote into the ring, calls `io_uring_enter`, copies the completions back); `membase` (the native address of wasm memory, which the kernel needs for buffer addresses); and `file_fd` (opens the database file read-only, so the WAT can submit an fsync of it to io_uring). It also starts one thread that calls `fdatasync` on the WAL file every 100 ms (below). There is no request logic in it |
| `src/sqlite_glue.c` | C, ~130 lines | `#include`s the stock SQLite amalgamation and exports what the WAT calls: the public SQL API; one-line wrappers over SQLite's B-tree cursor functions (open, first, next, previous, seek, payload); and, for checkpoints, a passive checkpoint that can skip its final sync of the database file, plus getters and a guarded setter for the WAL's frame count and `nBackfill`; and a wrapper on the page cache's rekey call (below). When to checkpoint and when that sync has happened are decided in the WAT |
| SQLite 3.54.1 | C, stock | The amalgamation, unmodified, compiled to wasm with wasi-sdk |
| wasi-libc | C | From wasi-sdk, for SQLite (malloc, file calls) |
| `experiments/` | notes, scripts, C | The worklog of every measured change, and `baseline/cserve.c`, the C reference server used to compare WAT with C compiled to wasm. Not built or run by the entry |

The WAT and SQLite are two wasm modules joined into one with `wasm-merge`; WasmEdge compiles the result
with LLVM (`wasmedge compile --optimize=3`) to a shared object that `host` loads.

## Versions

All pinned by version and sha256, installed by `install.sh` from official release tarballs, or
downloaded by `build.sh`:

- wabt 1.0.39 (`wat2wasm`), wasi-sdk 34 (clang for wasm32-wasip1), binaryen 125 (`wasm-merge`)
- WasmEdge 0.18.1 (AOT compiler and runtime library)
- SQLite 3.54.1 amalgamation
- gcc from Ubuntu 24.04 for `host.c`

## Design

- **One thread, io_uring, direct.** The WAT runs the loop through io_uring, with the host only copying
  submission and completion entries in and out (WASI's `poll_oneoff` would re-register every socket on
  every call). A multishot accept puts each new connection straight into a slot of the ring's
  fixed-file table, so connections never take a process file descriptor: with `LimitNOFILE=65535` the
  server still keeps every user's keep-alive connection, in up to 8 rings of 65,535 slots (320,000
  users tested). The accept moves to the next ring when one gets near full. Each connection has a
  multishot receive with buffer selection (2,048 x 2 KiB buffers per ring, in wasm memory). Ring 0 is
  the one waited on and holds a multishot poll on each other ring's fd.
- **Responses go out from one buffer.** After COMMIT, every response of the batch is a send straight
  from the batch's output buffer, all submitted in one `io_uring_enter`; with `MSG_DONTWAIT` they
  complete inside that call. A remainder the socket did not take is copied out and sent on its own;
  a send the kernel would retry later is waited for briefly and then cancelled (the connection is
  closed), so the buffer is never reused under a send. One send in flight per connection.
- **SQLite:** WAL, `synchronous=NORMAL`, persistent prepared statements, 64 MiB page cache,
  `temp_store=MEMORY` (WASI has no temp directory). WASI has no shared memory, so the connection runs with
  `locking_mode=EXCLUSIVE` (the WAL index lives in its heap); it is the only connection anyway.
  With no thread or fork in WASI, checkpoints run on the event loop, every 16,000 frames (64 MiB of
  WAL) instead of SQLite's default 1,000, so they come every few seconds.
- **WAL flusher thread.** A checkpoint starts by syncing the WAL, which by then holds up to 64 MiB, and
  the loop waits for it. A thread in `host.c` calls `fdatasync` on the WAL file (through its own
  descriptor) every 100 ms, so that sync finds little left to write. It only makes data durable sooner;
  SQLite still makes every sync it would make anyway. At 280,000 users this halved p95 and p99 (below).
- **The checkpoint's database sync goes to io_uring.** After a checkpoint copies pages into the
  database file, it fsyncs that file: 100-130 ms every ~3 s on the test box (bpftrace), with the loop
  stopped. The WAT now runs checkpoints itself (`wal_autocheckpoint=0`), after a batch's responses are
  sent, and has that one sync skipped: the copy runs as usual, then `nBackfill` (how much of the WAL is
  in the database file) is put back to its old value and an `IORING_OP_FSYNC` of the database file goes
  to the ring. The loop goes on serving; when the fsync completes, `nBackfill` moves up. Until then
  SQLite cannot restart the WAL, so every frame whose page is not yet durable in the database file is
  still in the WAL, and recovery after a crash replays it. Frames written during the sync get another
  round; once at most 2,000 are left, an ordinary checkpoint finishes them with its syncs inline (with the
  loop stopped for 90-157 ms on the test box: WAL 15 ms, database 40-57 ms, WAL header 4 ms, plus the
  copy) and the WAL restarts. The WAL's own sync before the copy is unchanged. `synchronous=NORMAL`
  holds throughout.
- **A page-cache fix in the glue.** perf showed `pcache1Truncate` at 1.2% of the CPU. Each commit calls
  it with the database's size, and about one commit in ten made it walk all 8,192 hash slots (~250 µs).
  The cause is SQLite's `balance_nonroot`, which swaps two pages through a temporary page number, the
  lock-byte page's (262,145 here), and the cache then believes it holds a page that far out. The glue
  wraps the cache's rekey call and keeps the old high-water mark for that one number; SQLite's source is
  unchanged. `pcache1Truncate` now shows 0.00% in the same profile.
- **Group commit.** One transaction per batch of completions: BEGIN after the wait, every request of
  the batch, then COMMIT, and only then are the responses sent. A transaction that has written stays
  open for up to 2 ms across waits to take in more writes; one that has only read commits at the end of
  its batch. If COMMIT fails, the batch's connections are closed unanswered, so no 201 is ever sent for
  a row that isn't committed. A generation counter per slot, carried in each entry's `user_data`, makes
  sure a held response or a late completion never reaches a new connection in the same slot.
- **Reads on SQLite's B-tree layer.** `/feed` and `GET /posts/:id` skip SQL's bytecode engine: the WAT
  opens cursors on `posts`, `users`, `posts_created_at_id_idx` and `likes_post_id_idx` (root pages read
  from `sqlite_schema` at start), seeks them, decodes the records itself, and counts likes with an index
  range scan. Same pager, page cache, WAL and locks; the schema cookie is checked in every transaction,
  and anything unexpected (a non-UTF-8 database, overflow pages, a changed schema) falls back to the SQL
  statements, which are always prepared. `POST /posts` and likes use SQL.
- **JSON** is written straight into the output buffer; string escaping checks 16 bytes at a time (SIMD).
  Short copies use plain loads and stores, because WasmEdge's AOT code turns each `memory.copy` into a
  call into the runtime (a lock, a bounds check and a memmove).
- **JWT:** HS256 verified on every request (SHA-256 and HMAC in WAT), `exp` checked; nothing is cached.

## Measured

**Scorer-like load, Synteq.** A rented 8 vCPU box with the grader droplet's CPU (Xeon Platinum 8260) and
kernel (6.8.0). The server runs alone on CPU 0 in a scope with `MemoryMax=2G`, with `LimitNOFILE=65535`;
the load generator (`ul`, C, the `bench/load.js` user loop with its real think times, one keep-alive
connection per user, 32 source addresses) runs on CPUs 1-7. 60 s ramp, then the measured window.
`experiments/worklog.md` has every run, including the changes that didn't help.

5-minute hold at 320,000 users with the io_uring database sync: 33.7k req/s, p50 8.9 ms, **p95 198 ms,
p99 455 ms, 0 errors**, max 1.8 s; the server used 94% of its CPU (27.8 µs per request) and peaked at
880 MB (cgroup, page cache included). 320,000 is this box's edge, though: later the same day, paired holds
gave p95 295 and 280 ms / p99 835 and 765 ms for that build, and p95 275, 265 and 365 ms / p99 800, 785
and 1,165 ms for this commit (the last one over the p99 limit), all at 93-95% CPU and 0 errors. 330,000
and 340,000 users fail on CPU (97-98%). Before the checkpoint change, the 5-minute hold at 280,000 users gave p95 355 ms
and p99 670 ms.

2-minute runs, 3 interleaved reps each:

| server | users | p95 (ms) | p99 (ms) | CPU per request |
|---|---|---|---|---|
| io_uring loop, no WAL flusher | 280,000 | 325, 385, 425 | 620, 735, 800 | 27.7-27.9 µs |
| + WAL flusher | 280,000 | 196, 138, 184 | 465, 285, 390 | 28.7-28.9 µs |
| + WAL flusher | 300,000 | 280, 230, 265 | 560, 440, 520 | 27.3-28.0 µs |
| + database sync through io_uring | 300,000 | 134, 127, 109 | 345, 310, 250 | 28.5-29.0 µs |
| same, rerun later, paired with the next row | 300,000 | 76, 67, 147 | 210, 155, 420 | 27.3-27.7 µs |
| + page-cache fix (this commit) | 300,000 | 86, 59, 78 | 215, 132, 176 | 27.1-27.8 µs |
| objc-uring-delneg, for reference (1 run) | 280,000 | 11.2 | 23.4 | 25.5 µs |

At 320,000 users the server is at 91-94% of its one CPU, so CPU per request is the next limit.

The ObjC entry checkpoints from a separate process, so its loop never waits for an fsync; that is most
of the gap in the tail. It uses about 0.9x this entry's CPU per request here.

The build merged in #259 (epoll, one file descriptor per connection) had to close idle keep-alive
connections past ~60,000 users; the io_uring loop keeps all of them.

**CPU per request, opt-box.** The numbers below are older, from opt-box (a 4 vCPU VM whose disk takes
~17 ms per fsync) and the earlier epoll loop; the io_uring loop measures the same there (35.8k, 36.6k,
35.8k req/s, 10.9-11.2 µs per request). The server runs on CPU 0, a load generator on CPUs 1-3, with think
times multiplied by a scale factor.

Throughput with the server held to 40% of one CPU (`systemd-run --scope -p CPUQuota=40%`), 4,000 users
at scale 0.007 (more than any of them can serve), 3 interleaved runs:

| server | req/s | CPU per request | p95 |
|---|---|---|---|
| this entry (WAT) | 35.3k, 35.8k, 35.8k | 11.2-11.3 µs | ~105 ms |
| objc-uring-delneg | 49.5k, 49.7k, 48.7k | 8.1 µs | ~45 ms |
| c-iouring-public | 50.4k, 49.9k, 48.5k | 7.9-8.0 µs | ~46 ms |

So this entry does about 0.72x of either io_uring entry's work per CPU second. Both read `/feed` and
posts through SQLite's B-tree layer the same way, natively (ObjC also with PGO), and their kernel time
per request is close to this entry's. This entry's user time is about 2x theirs: SQLite and the WAT
running as wasm. They also checkpoint the WAL off the event loop (a forked child, a thread). WASI here
has neither, so checkpoints run inline and their fsyncs pause the loop: at ~30k req/s, p95 is ~38 ms
against 1.5-1.9 ms for the two baselines. That is well inside the score's 500 ms.

**WAT vs C compiled to wasm.** With the same epoll loop, the same host and the same SQL statements, the
WAT server and the C reference server compiled to wasm used the same CPU per request (7.9 against
7.7 µs on an older, lower-reading metric, within noise). The C reference as it is (SQL only, each write
its own transaction, no batched host calls) serves 15.3k-15.9k req/s in the test above. The lead over
that build comes from the design; the language made no measurable difference.

How the CPU per request came down (perf task-clock, 4,000 users at scale 0.013, ~30k req/s):

| change | µs per request |
|---|---|
| group commit, one transaction per epoll batch | 21.2 |
| `/feed` and `GET /posts/:id` on the B-tree layer | 15.3 |
| writes held up to 2 ms for more of them | 14.4 |
| SIMD JSON escaping | 13.6 |
| one host call to send a whole batch | 13.2 |
| one host call to wait and receive | 13.0 |
| binaryen `wasm-opt -O3` before AOT | 12.8 |
| `$put` without `memory.copy` | 12.6 |
