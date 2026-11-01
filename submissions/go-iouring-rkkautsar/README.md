# Go io_uring + direct SQLite

**Language:** Go 1.27.1
**Transport:** custom HTTP/1.1 parser over raw Linux io_uring; `net/http` is used only for status constants and the optional pprof listener
**Driver:** direct cgo against the vendored SQLite 3.46.1 amalgamation (`sqlite3.c`/`sqlite3.h`), built with `SQLITE_THREADSAFE=2` and `SQLITE_OPEN_NOMUTEX`
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx

All five endpoints read SQLite while the request is being served. There is no response
cache, query-result cache, in-memory table copy, or remembered JWT verification. Groups of
concurrent reads share only their in-flight SQLite result, and the group is removed as soon
as that read completes. Prepared statements cache only query plans, as allowed by rule 5.

## Architecture

- **Raw io_uring is the default.** The process starts three `SO_REUSEPORT` listener shards.
  Each shard owns an io_uring, a 131,072-entry registered file table, its HTTP parser, and
  its output queues. Accepted sockets use `IORING_FILE_INDEX_ALLOC`, so accepted
  connections do not consume the process `RLIMIT_NOFILE` budget. Sparse file-table
  registration is requested first; if the kernel rejects it, a normal registered table is
  used, and if ring setup fails the process falls back to the Go connection loop.
- **One DB owner per shard.** Feed requests, `GET /posts/:id` batches, and create/like
  batches are sent to the shard's blocking DB worker. There is no goroutine per connection
  and no per-request transport goroutine. Each worker batches concurrent feed requests into
  one query, post IDs into one `IN` query, and writes into one transaction.
- **Durable writes.** The raw writer uses `BEGIN IMMEDIATE`/`COMMIT` and only queues a
  201/200 response after the commit succeeds. SQLite runs WAL with `synchronous=NORMAL`;
  the schema is unchanged. The Go fallback group-commits writes every `WRITE_BATCH_MS`
  (default 100 ms), and every response waits for its commit.
- **Multishot transport.** Accept and receive are multishot io_uring operations. Receives
  use registered provided-buffer rings with two 32,768-buffer groups of 1 KiB per shard.
  A receive is re-armed only when `IORING_CQE_F_MORE` is absent or the ring reports
  `-ENOBUFS`; consumed buffers are returned by advancing the buffer-ring tail. There is no
  epoll descriptor and no eventfd: the worker wakes the event loop through a pipe read SQE.
- **Buffer lifetime.** Input and output slices are released when empty. The raw path builds
  complete HTTP packets, so a normal response uses one send. `SEND_ZC` was measured and
  made throughput worse, so plain `IORING_OP_SEND` is used.
- **Parser.** The request line and only the headers this API needs (`Authorization`,
  `Content-Length`, `Connection`) are parsed into a small struct. The tested surface is
  Content-Length requests from curl/k6; chunked bodies and unusual methods are outside it.
- **SQLite queries.** `/feed` and single-post reads use SQLite's `json_object`/
  `json_group_array` functions and append the returned JSON fragment to the envelope.
  `loadPosts` prepares one `IN` query per batch; statements do not cache rows. `like_count`
  is counted from `likes` in the request with a correlated subquery. Read connections use
  `cache_size=-16384` (16 MiB) and `mmap_size=268435456` (256 MiB); these are SQLite page
  cache and mmap, not application caches.
- **JWT.** HS256 is verified directly with `crypto/hmac`; only HS256 is accepted, `exp` is
  checked, and the signature is verified for every request. There is no token cache.
- **Fallback.** `URING=0` selects the Go `net`-based connection loop and read-coalescing
  path. It is available for diagnosis and for kernels where the raw path cannot initialize;
  the raw path is what is benchmarked.

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required by the challenge | SQLite database file |
| `JWT_SECRET` | required by the challenge | HS256 signing secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `URING` | unset/`1` | `0` selects the Go fallback |
| `READ_CONNS` | `2` | SQLite read connections |
| `READ_BATCH_US` | `500` | Go fallback read window |
| `WRITE_BATCH_MS` | `100` | Go fallback write commit window |
| `PPROF_ADDR` | unset | Optional pprof listener |

## Local measurements

These are local emulations on one pinned CPU with a separate set of k6 clients, not the
official benchmark. The database was a fresh copy of the seed on every run.

| Run | p95 | p99 | Server CPU | Peak RSS | Errors |
|---|---:|---:|---:|---:|---:|
| 200k VUs, 20 s mixed load | 1.89 s | 2.20 s | 94.5% | 1.131 GiB | 0 |

The 200k run still fails the challenge latency thresholds locally. The remaining cost is
kernel/TCP and io_uring work per response plus the SQLite read path; more receive buffers,
more buffer groups, zero-copy receive, zero-copy send, larger socket buffers, and write
batching were all measured and did not help. The design is comfortably within the 2 GiB
memory limit, and the local result is around the limit of this Go/cgo/SQL architecture.
A 280k result would require a different read path, specifically a rule-compatible B-tree
cursor implementation and/or a C service loop, not more buffer tuning.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` (cgo); SQLite is vendored, so no system
  SQLite development package is installed.
- `build.sh` — cgo build of the vendored amalgamation, trimpath and stripped, to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
- `dev/` — optional local Docker/k6 load scripts used during development; not used by CI.

## Verification

`bash test/run.sh submissions/go-iouring-rkkautsar` passes all 42 checks on a fresh seed
database copy.
