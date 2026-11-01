# Twelve Dollar Server Challenge — Cider io_uring v3 (native B-tree reads)

A Cider HTTP server built around `io_uring`, SQLite 3.54.1, and OpenSSL.
The application and HTTP/JSON logic are written in Cider. Two small C leaf
helpers provide the parts that Cider cannot express directly:

- `src/btree.c` includes the pinned SQLite amalgamation and implements native
  SQLite B-tree read plans. The benchmark owner explicitly allowed direct
  B-tree access after the Lean submission demonstrated it.
- `src/fast.c` provides the SSE2 JSON string escape and integer formatter.

There is no C server/framework wrapper, no external service, and no
cross-request response or query cache. Every request reads its data from
SQLite while it is being served. SQLite's page cache, mmap, prepared
statements, and WAL are used as allowed by the rules.

## Measured result

Cloud benchmark on Synteq hardware equivalent to the challenge box:

- server: 1 vCPU, 2 GB RAM, Ubuntu 24.04, no swap, direct on `:80`
- load: 4 vCPU / 4 GB, five source IPv4 addresses, custom C `epoll` generator
- fresh seed database, **42/42 challenge tests passing**
- 120 s ramp, 300 s hold, same user loop as `bench/load.js`

| users | requests | HTTP failures | CPU/request | hold p50 | hold p95 | hold p99 |
|---:|---:|---:|---:|---:|---:|---:|
| **250,000** | 11,453,808 | 244 (0.002%) | **25.11 µs** | 76 ms | **321 ms** | **420 ms** |
| **220,000** | 10,116,904 | 221 (0.002%) | **26.00 µs** | 2 ms | **305 ms** | **381 ms** |
| **200,000** | 9,219,851 | 223 (0.002%) | **26.10 µs** | 1 ms | **257 ms** | **320 ms** |
| 200,000 (v2 SQL reads) | 9,162,496 | 130 (0.001%) | 32.42 µs | 71 ms | 322 ms | 397 ms |
| 150,000 (v2, 4 workers) | 6,931,842 | 130 (0.002%) | 36.85 µs | 6 ms | 129 ms | 162 ms |
| 135,000 (v1 baseline) | 6,244,233 | 105 (0.002%) | 38.44 µs | 1 ms | 122 ms | 158 ms |

The **250,000-user hold** passes all thresholds (`p95 < 500 ms`,
`p99 < 1000 ms`, HTTP errors `< 1%`). Total percentiles for that run were
p95 310 ms and p99 406 ms; connection failures were 8 (0.00007%), also below
1%. The 220k hold was p95 305 / p99 381 ms, and the 200k run total was
p95 250 / p99 314 ms with 4 connection failures.

An AVX-512 variant of the JSON escape was also built and tested. It measured
p95 252 ms / p99 311 ms at 27.06 µs/request, versus SSE2 at p95 257 ms /
p99 320 ms and 26.10 µs/request. The AVX-512 path was rejected for the final
build because its wider instructions did not improve request CPU; SSE2 is the
shipped path.

## What changed from v2

- **Native B-tree reads.** The feed and single-post read paths now use
  `sqlite3BtreeCursor`, `sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`,
  `PayloadFetch`, and the record decoder directly. This skips the SQLite VDBE
  for reads while SQLite still owns the pager, WAL, transactions, locking, and
  durability. Writes remain ordinary SQL and group-commit under WAL.
- **SSE2 JSON escaping.** `fast_append_json_string()` scans 16 bytes at a time
  for quotes, backslashes, and control bytes while preserving the exact JSON
  escape rules. An AVX-512 version was measured and rejected.
- **Four io_uring workers** at the official `LimitNOFILE=65535`, instead of the
  original eight-worker configuration.
- **No `sqlite3_clear_bindings()`** on hot statements. SQLite marks statements
  with an `expmask` expired when bound values change; clearing bindings caused
  the GET-post query to reparse on almost every request. Removing the clears
  stopped that reprepare storm.
- **Repeated likes** are detected with a primary-key read before opening a
  write transaction. If `(user_id, post_id)` already exists, the server returns
  `200` with `already_liked: true` without touching the write path.
- SQLite tuning: 256 MB page cache, `cache_spill=OFF`, 1 GB mmap, WAL with
  `synchronous=NORMAL`, and a manual WAL checkpoint every 8,192 frames. Writes
  remain durable in the WAL before their responses are sent.

## Architecture

- Four `io_uring` rings, one worker each; multishot accept and multishot recv,
  provided-buffer rings, fixed file descriptors, and pooled response buffers.
- One SQLite connection and one global database mutex. CQEs from all workers
  are processed serially, which keeps SQLite single-threaded and gives a
  natural group-commit boundary at the end of each CQE batch.
- Feed reads use a reverse rowid scan of the latest 20 posts, one
  `likes_post_id_idx` range scan for the like counts, and user rowid seeks.
  There is no feed cache.
- JWT HS256 signatures are verified with OpenSSL on every authenticated
  request; no token verification is remembered across requests.

## Rules

- SQLite is opened in-process at `$SQLITE_PATH`; no other database or cache
  service is used.
- The schema is unchanged. The B-tree plans read the existing tables and
  indexes only.
- Every request reads SQLite; there is no cross-request response/query cache.
- Writes use WAL with `synchronous=NORMAL` and are committed before responses
  are released.
- No hard-coded responses or load-test detection.
- The server binds directly on `:80` as an unprivileged user with
  `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE`.
- Direct SQLite B-tree reads are used with the benchmark owner's explicit
  permission.

## Build

```bash
sudo bash install.sh          # installs the pinned Cider toolchain
bash build.sh                 # builds SQLite + B-tree plans, Cider source, helpers
```

`build.sh` pins the SQLite amalgamation version and checksum. The compiler is
pinned to the upstream revision used by the challenge; no compiler patch is
required.

## Run

```bash
export SQLITE_PATH=/path/to/feed.db
export JWT_SECRET=twelve-dollar-challenge
PORT=80 bash start.sh
```

The process must run as an unprivileged user with `LimitNOFILE=65535` and
`CAP_NET_BIND_SERVICE` when binding port 80.

## Test

```bash
PORT=8080 bash test/run.sh submissions/cider-iouring-pomilon-v3
```

The suite covers all five endpoints, authentication failures, validation,
special-character round-trips, the 500-character post limit, likes, and feed
ordering. It passed **42/42** locally and **42/42** on the cloud server.
