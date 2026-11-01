# WebAssembly, hand-written (WAT) / WasmEdge AOT / SQLite

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx). The server is written by hand in WebAssembly's text format,
compiled ahead of time to native code by WasmEdge, and run by a small C embedder.

## What is WAT and what is C

| File | Language | What it does |
|---|---|---|
| `src/app.wat` | WAT, hand-written | Everything a request touches: the epoll loop, HTTP/1.1 parsing, routing, JSON reading and writing, base64url, SHA-256 and HMAC for the HS256 JWT check, SQL statement calls, group commit, idle-connection eviction, and the B-tree read paths (record decoding, which cursors to open and seek) |
| `src/host.c` | C, ~125 lines | WasmEdge embedder: loads the AOT-compiled module, gives it WASI (used only by SQLite for file I/O, env vars and the clock) and ten Linux calls as imports: `listen`, `epoll_create`, `epoll_ctl`, `epoll_wait`, `accept4`, `recv`, `send`, `close`, and two batched ones: `epoll_recv` (epoll_wait, then one `recv` per readable socket) and `sendmany` (one `send` per response record). Each import moves its arguments between wasm memory and syscalls; there is no request logic in it |
| `src/sqlite_glue.c` | C, ~70 lines | `#include`s the stock SQLite amalgamation and exports what the WAT calls: the public SQL API, and one-line wrappers over SQLite's B-tree cursor functions (open, first, next, previous, seek, payload). No decisions are made here |
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

- **One thread, epoll, direct.** The WAT runs its own epoll loop through the host's `sys` imports
  (WASI's `poll_oneoff` would re-register every socket on every call). Host calls are batched: one call
  waits and receives on every readable socket of the batch, one call sends all of the batch's responses.
- **Idle sockets near the fd limit.** With `LimitNOFILE=65535` and one keep-alive socket per user, the
  server closes sockets idle for 1.5 s or more once it holds over 60,000; `bench/load.js` never waits
  less than 3 s between a user's requests, so a closed idle socket is simply reopened by the client.
- **SQLite:** WAL, `synchronous=NORMAL`, persistent prepared statements, 64 MiB page cache,
  `temp_store=MEMORY` (WASI has no temp directory). WASI has no shared memory, so the connection runs with
  `locking_mode=EXCLUSIVE` (the WAL index lives in its heap); it is the only connection anyway.
- **Group commit.** One transaction per epoll batch: BEGIN after `epoll_wait`, every request of the batch,
  then COMMIT, and only then are the responses sent (all of them, from one buffer, in one host call). A
  transaction that has written stays open for up to 2 ms across `epoll_wait` calls to take in more
  writes; one that has only read commits at the end of its batch. If COMMIT fails, the batch's
  connections are closed unanswered, so no 201 is ever sent for a row that isn't committed. A per-fd
  generation counter makes sure a held response never goes to a new connection that reused the fd.
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

All numbers are local, from opt-box (a 4 vCPU VM whose disk takes ~17 ms per fsync), not from the
grader's droplet. The server runs on CPU 0; a load generator runs the `bench/load.js` user loop on
CPUs 1-3, with its think times multiplied by a scale factor. `experiments/worklog.md` has every run,
including the changes that didn't help.

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
