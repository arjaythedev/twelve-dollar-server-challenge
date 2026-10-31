# Rust native feed server (rust-native-M3M0KA)

Clean-sheet Rust implementation of the feed API. No framework, no async runtime,
no third-party crates. HTTP/1.1 framing, JSON validation, SHA-256/HMAC and the
SQLite bindings are written in this repository.

**Serving topology: direct** on `HOST:PORT` (`0.0.0.0:80` on the benchmark box). No Nginx.

## Build and run

    sudo bash install.sh        # Ubuntu 24.04: C toolchain + pinned Rust 1.94.0 under /opt
    bash build.sh               # pinned SQLite 3.54.1 amalgamation (sqlite-amalgamation-3540100), checksum-verified, PGO
    SQLITE_PATH=/path/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh

## Files

- `src/main.rs` - configuration, epoll transport, worker processes (`FEED_WORKERS`, default 5),
  event-loop pass = one transaction (write lock taken up front when the pass contains a POST)
- `src/net_uring.rs`, `src/uring.rs` - io_uring transport: six rings of 65,535 direct
  descriptors, multishot accept and receive, provided-buffer rings. Used only after a
  live self test (`self_test`) succeeds; otherwise the epoll transport serves.
- `src/app.rs` - routes, validation, JWT verification, response framing
- `src/db.rs` - SQLite connection, SQL statements, batch transaction, startup checks
- `src/btree.rs`, `src/sqlite_shim.c` - B-tree read plans for `/feed` and `/posts/:id`
  (the shim is compiled into the same translation unit as the unmodified amalgamation)
- `src/uring.rs` - raw kernel UAPI bindings, with byte-level checks for every
  submission entry the server uses (`rustc --test src/main.rs`, 6 tests)
- `src/http.rs`, `src/json.rs`, `src/crypto.rs`, `src/sys.rs` - protocol primitives
- `src/pgo_train.c` - build-time profile training on a synthetic database (not the seed)
- `tests/oracle.sh` - differential test: B-tree plans vs pure SQL, byte-identical, before and after writes
- `bench/load.rs` - development load generator (not shipped in the image)

## Read plans

`GET /feed` walks `posts_created_at_id_idx` for its first 20 entries, which is
exactly the `ORDER BY created_at DESC, id DESC` order the API asks for, and reads
the rows from the `posts` tree (stepping back with `sqlite3BtreePrevious` when the
ids are consecutive), the authors from `users`, and the like counts from
`likes_post_id_idx` (one range scan when the 20 ids are consecutive, one seek per
id otherwise). `GET /posts/:id` is a rowid seek plus the same author and count
lookups.

Nothing is remembered between requests: no next id, no cached timestamp, no data
version. A write by this server, by a restart, or by any other connection is simply
read on the next request, so the plan never has to switch itself off — the defect
that turned this server's reads back into SQL under the scored load. At startup the
plan's output is compared byte for byte with the reference SQL and disabled only if
they differ.

## Correctness

- `test/run.sh` (the official 42 checks): **passed 42/42** on this build.
- `tests/oracle.sh`: plan output identical to pure SQL on a fresh seed and after writes.
- The B-tree plan checks the schema layout and the feed invariant (dense rowids,
  non-decreasing `created_at`) at startup and after every insert. Any violation, foreign
  write, or unreadable record disables the plan permanently and reads use SQL.

## Connection capacity

One process cannot hold more than its descriptor limit (65,535 under the challenge's
`LimitNOFILE`). The epoll transport therefore forks `FEED_WORKERS` (default 5) processes
after binding one listening socket. Each worker has its own descriptor table, its own
SQLite connection and its own epoll set; the kernel distributes accepts among them.
Five workers give 327,675 descriptors. io_uring, when its self test passes, serves from a
single process instead.

## Durability

Each event-loop pass runs in one SQLite transaction. Responses are written only after
`COMMIT` returns. If the commit fails, every response in the pass is replaced by a
500 and its connection is closed. WAL with `synchronous=NORMAL`.

## Measured (local, not official)

Single core, server pinned with `taskset`, closed-loop clients on other cores, seed
database, app CPU from `/proc/<pid>/stat`. The same build measured before the
per-request allocator and buffer fixes was 20.5 us/request on `/feed`:

| endpoint | app CPU per request |
|---|---|
| `GET /health` | 3.4 us |
| `GET /posts/250000` | 3.4 us (293,000 req/s on one core) |
| `GET /feed` | 9.6 us |
| `GET /feed`, `FEED_NO_PLAN=1` | 28.5 us |

`strace -c` on the serving thread shows one `recvfrom` and one `sendto` per request
and nothing else per request: the connection buffers keep their capacity, so the
allocator does not enter its mmap/munmap path (that was 2 syscalls per request).

Multi-process capacity, server limited to `ulimit -n 65535` per process, five workers,
clients from 38 loopback source addresses:

- 300,000 idle keep-alive sockets held; four workers at 65,535, one at 37,900; about
  88 MB user memory per worker; 0% CPU while idle.
- Official `bench/load.js`, 3,000 VUs, 60 s, five workers: checks 100%, 0 failed requests,
  p99 1.57 ms, max 149 ms.
- Read-your-writes across workers: a post created through one connection was read back
  through 30 new connections, each served by whichever worker accepted it.

Not measured here: kernel socket memory at 300,000 connections on the 2 GB box (the
user-space figure above excludes it), and CPU contention of five processes on one vCPU
at full load.

## Measured (single process, earlier)

Host: 20-core container, loopback, client and server on the same machine.
Single-thread server, closed-loop clients, `/feed` and `/posts/:id`:

| run | req/s | p99 |
|---|---|---|
| `/feed`, 256 conns | ~45,000 | 6.5 ms |
| `/posts/250000`, 256 conns | ~70,000 | 4.5 ms |
| official `bench/load.js`, 3,000 VUs, 60 s | 239 (think-time bound) | 1.05 ms, 0% failed |

Connection capacity: the epoll transport held **300,000 concurrent keep-alive sockets**
with the process descriptor limit raised (`ulimit -n 1048576`) and returned `200 OK`.
The grader's `LimitNOFILE=65535` caps the epoll transport at about 65,500 connections.
Past that the io_uring transport is required. **This container denies `io_uring_setup`
(EPERM), so the io_uring path has not been exercised here**; the server falls back to epoll
and says so in its log (`transport: epoll`).

## Limitations

- The io_uring transport is unverified on a kernel that permits it. Its self test is the
  guard: if it fails, the server keeps serving on epoll.
- Writes in one worker make the B-tree plan of the other workers stop (their
  `data_version` changes). They then read through SQL, which is correct but slower.
  Re-validating the feed tail after a foreign write is the next optimisation.
- No official leaderboard score exists for this build. Ranking is decided by the maintainer's
  benchmark on the 1 vCPU / 2 GB box.
