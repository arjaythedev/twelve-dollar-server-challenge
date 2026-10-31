# C, io_uring, SQLite's B-tree cursors, one thread

One event-loop thread serves every connection through io_uring and every request through one
SQLite transaction per batch. Reads walk SQLite's own B-tree cursors inside that transaction;
writes are prepared SQL with group commit. Nothing on the request path allocates.

| | |
|---|---|
| Language | C11, gcc 13 (Ubuntu 24.04 `build-essential`), one translation unit with the SQLite amalgamation |
| Framework | none: own io_uring layer (raw syscalls, no liburing), HTTP/1.1 framing, JSON, HS256 |
| Database | SQLite 3.54.1 amalgamation, SHA-256 pinned, downloaded by `build.sh`; `unix-excl` VFS |
| Build | `-O3`, static, profile-guided (trained on a synthetic database at build time), AVX-512 kept to 256-bit |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with provided buffer rings and multishot accept/receive); tested on Ubuntu 24.04's 6.8 |

## Connections

Every user is a keep-alive connection, so the connection count is what the server is sized for.

- Accepted sockets are io_uring direct descriptors, held in each ring's fixed-file table rather than
  the process's descriptor table, so `LimitNOFILE=65535` does not cap them. A table cannot be larger
  than that limit, so the server makes 8 rings of 65,471 slots (over 500,000 connections).
- Each ring has its own `SO_REUSEPORT` listener with a multishot accept that is always armed, so a
  full table on one ring never stops accepts on the others.
- Each connection has one multishot receive into a shared provided-buffer ring (2,048 × 2 KB per
  ring); an idle connection holds no buffer and no heap, only a 48-byte record. The kernel's own
  per-socket memory (~4.3 KB on 6.8, most of it not charged to the process) is the real ceiling on
  a 2 GB box, and the 48 bytes are what the server adds to it.
- Ring 0 is `SINGLE_ISSUER | DEFER_TASKRUN` and is the ring the loop waits on (a timed
  `io_uring_enter`: 64 completions or 2 ms when busy, up to 1 s when idle). The other rings use
  ordinary task work and signal an eventfd that ring 0 polls, so a completion anywhere ends the
  wait and idle rings cost nothing. The eventfd is never read back: a multishot poll posts one
  completion per wake-up, so draining the counter was 0.3 system calls per request for nothing.
  (With `DEFER_TASKRUN` on every ring, a completion on a ring
  that is not being entered stays unposted and the loop sleeps through it: measured as a 100–500 ms
  tail on the second request of a connection.)
- Ubuntu's 6.8.0-136 and later kernels reject provided-buffer-ring registration with `EINVAL`
  (Launchpad #2162843: an inverted reserved-field check). The server retries once with a nonzero
  reserved byte, which those kernels accept and a correct kernel never sees.
- Idle keep-alives are closed after 90 s without a request; closes are a shutdown hard-linked to a
  close of the slot.

## A batch

Each wake-up of the loop is one batch:

1. Every completion is handled. Requests are parsed in place in the kernel's receive buffers and
   answered straight into a per-batch arena. A connection gets at most one request per batch; any
   pipelined bytes wait for the send to complete.
2. All of the batch's reads and writes run in one SQLite transaction, begun lazily by the first of
   them (group commit). Reads see the batch's earlier writes.
3. `COMMIT`, then every response is handed to the kernel as one send pointing into the arena. If
   the commit fails, every response that touched the database becomes a 500. No 201 ever claims
   a write that did not land, and no read ever reports a row that might roll back.

## SQLite

- `GET /feed` and `GET /posts/:id` run on SQLite's B-tree cursors (`sqlite3BtreeFirst/Next`,
  `sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`) inside the batch's read transaction. The
  feed takes the 20 newest ids off `posts_created_at_id_idx`, counts all 20 posts' likes with one
  range scan of `likes_post_id_idx`, reads the rows with one range scan of `posts` (a single seek
  to the lowest id, then `sqlite3BtreeNext`, as the bytecode engine runs `WHERE id BETWEEN`), then
  each author by rowid. Every page comes
  through SQLite's pager, WAL and page cache; what is skipped is the bytecode interpreter, since
  both queries are known ahead of time. The stored layout is checked at startup against the schema
  the plans were written for, and any surprise during a read falls back to the reference SQL. The
  organiser allowed this reading of rules 3/5 on 2026-10-30 (PR #153).
- Writes are prepared SQL. `INSERT` + `last_insert_rowid` + `SELECT created_at` is cheaper than
  `RETURNING`; likes are `INSERT … SELECT … WHERE EXISTS … ON CONFLICT DO NOTHING`.
- `unix-excl` VFS: this process is the only one that opens the file, so the wal-index lives in heap
  memory and no `fcntl()` lock is taken per transaction. The checkpoint thread's second connection
  shares it (same process).
- A small VFS shim over `unix-excl` (the pattern of SQLite's own `vfstrace`/`multiplex` shims,
  public API only) writes each WAL frame with one `pwrite` instead of two: SQLite writes a frame's
  24-byte header and then its page at the next offset, and the shim holds the header for that one
  call and sends both together. Any other call on the WAL file writes a held header first, and a
  reader finds a frame only after the wal-index is updated, after both halves are written. WAL
  `pwrite`s on the request thread: 0.32 → 0.16 per request.
- System calls per request on the request thread, counted with kernel tracepoints at 120,000
  users: 0.44 (`io_uring_enter` 0.28, WAL `pwrite` 0.16, everything else ≈ 0). The checkpoint
  thread adds 0.17, off the request path.
- WAL, `synchronous=NORMAL`, `wal_autocheckpoint=0`. A WAL hook wakes a checkpoint thread past
  1,000 frames (PASSIVE, on its own connection, never on the loop). Past 4,000 frames, writes are
  held until the thread reports the log fully copied (at most 1 s; reads go on), so the next write
  restarts the log and it stays small. Past 16,384 frames the loop checkpoints itself, as a last
  resort.
- No memory map (`mmap_size=0`) and a 32 MB page cache. With a memory map every page fetch first
  probes the WAL index; with the pages in SQLite's own cache a hit returns at once (measured at
  120,000 users: 17.6 → 15.5 µs per request). SQLite's own counters (`SQLITE_DBSTATUS_CACHE_*`,
  logged every 10 s) put the working set at about 34 MB: at 120,000 users a 32 MB and a 64 MB cache
  both miss only on first load after startup (~4,300 misses in 55 million lookups) and cost the
  same CPU within noise, so the smaller one is kept for the memory it leaves to connections.
  `THREADSAFE=2` with `OPEN_NOMUTEX`; memory statistics, the progress-callback check per opcode,
  shared cache and unused features compiled out.
- SQLite's default page cache sits behind its plugin interface (`SQLITE_CONFIG_PCACHE2`) with one
  change: at commit, when no page past the end of the file can exist, the cache does not walk its
  hash table to drop them. A page split briefly renames a page to the lock-byte page's number,
  which made every later commit scan the whole cache (1.8% of CPU in a profile); the wrapper
  tracks the keys that enter the cache and skips that scan, 9,770 times out of 9,772 in a test run.
  The database passes `PRAGMA integrity_check` afterwards and every read matches SQL.

## HTTP, JSON, JWT

- Only the headers the API uses are looked at (`memmem` for the header end, then a case-insensitive
  match on `Authorization`, `Content-Length`, `Connection`, `Transfer-Encoding`, `Expect`).
- Every error response is a constant prepared at startup. Dynamic responses are built body-first in
  the arena and the status line and headers are written in front of them, so there is no copy and no
  `Content-Length` placeholder.
- JSON output escapes only what JSON requires, scanning 8 bytes at a time; integers use a two-digit
  table. Input JSON is fully validated (the body and the token payload).
- HS256: SHA-256 with the key's inner and outer states computed once from `JWT_SECRET` (the key
  schedule, not a verification result). Every token is verified on every request: signature
  (constant-time compare), `alg` must be `HS256`, `exp` required and in the future; then `sub` must
  be a positive integer string and `username` a string.

## Build

`install.sh` installs `build-essential`, `curl`, `unzip`. `build.sh` downloads the pinned amalgamation,
builds an instrumented binary, generates a synthetic database (5,000 users, 40,000 posts, 160,000
likes of its own random data, no seed data) and drives the load test's request mix plus the test
suite's error paths through it with `src/train.c`, then builds the final binary with the profile.
`PGO=0 ./build.sh` skips that. Flags: `-O3 -march=x86-64-v3 -mtune=cascadelake
-mprefer-vector-width=256` (no 512-bit instructions: on Cascade Lake one would lower the core's
frequency for the next ~0.5 ms), `-static -fno-plt`, Ubuntu's default hardening off.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process, schema untouched. Reads use SQLite's own B-tree
cursors through SQLite's pager; writes use SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: no response, query-result or token caches. Every request reads SQLite
pages while it is served. Kept between batches: prepared statements, the table root pages and column
layout (schema metadata, read at open), and the HMAC key schedule.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for its batch's commit.
7. Nothing detects the load test.
8–9. Direct on `0.0.0.0:80`. Direct descriptors need no file descriptors beyond the listeners.
10. Built from source on the box; SQLite 3.54.1 pinned by SHA-256.
11. Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Layout

```
src/sys.h     raw io_uring ABI: rings, direct descriptors, buffer rings, SQE builders
src/server.c  the event loop, batches, arenas, connection table; includes sqlite3.c
src/app.h     the five endpoints
src/db.h      SQLite: transactions, statements, the checkpoint thread and write gate
src/btree.h   the read plans on SQLite's B-tree cursors
src/http.h, json.h, jwt.h, sha256.h
src/train.c, train.sh   PGO training client and driver (build time only)
```

## Local measurements (not a score)

Ubuntu 24.04 arm64 VM (Lima, kernel 6.8.0-147), server pinned to one vCPU under systemd with
`MemoryMax=1750M` and `LimitNOFILE=65535`, a native replay of `bench/load.js` on two other vCPUs
over loopback. Numbers are only comparable with each other; loopback charges part of the client's
TCP work to the server, and the vCPU is not the benchmark's.

| Users | Ramp / hold | Requests | Errors | p95 | p99 | max | Server CPU per request | Process RSS | cgroup peak |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 20,000 | 60 s / 120 s | 328k | 0 | 3.2 ms | 4.7 ms | 632 ms | 31 µs | — | 118 MB |
| 120,000 | 90 s / 120 s | 2.16M | 0 | 6.1 ms | 14.0 ms | 45 ms | 17 µs | 73 MB | 304 MB |
| 220,000 | ramp only | 1.3M | 0 | 9.2 ms | 23 ms | — | ~19 µs | 83 MB | 480 MB |

The 220,000 row is the point where the load generator ran the 4 GB VM out of memory (both ends of
440,000 sockets live in it); the server itself was at 21,800 requests/s with ~118 requests per
wake-up and a longest turn of 20 ms.

At 120,000 users the loop handled ~65 requests per wake-up and its longest turn was 18.5 ms; the
cgroup grows by about 1.9 KB per connection, nearly all of it the kernel's per-socket objects.
