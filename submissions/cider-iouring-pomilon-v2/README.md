# Twelve Dollar Server Challenge — Cider io_uring v2

A pure-Cider HTTP server built around `io_uring`, SQLite 3.54.1, and OpenSSL.
The hot path has no cross-request response or query cache: every request reads
its data from SQLite while it is being served. SQLite's page cache, mmap, and
prepared statements are used, as allowed by the challenge rules.

## Measured result

Cloud benchmark on Synteq hardware equivalent to the challenge box:

- server: 1 vCPU, 2 GB RAM, Ubuntu 24.04, no swap, direct on `:80`
- load: 4 vCPU / 4 GB, five source IPv4 addresses, custom C `epoll` generator
- fresh seed database, **42/42 challenge tests passing**
- 120 s ramp, 300 s hold, same user loop as `bench/load.js`

| users | requests | HTTP failures | CPU/request | hold p50 | hold p95 | hold p99 |
|---:|---:|---:|---:|---:|---:|---:|
| **200,000** | 9,162,496 | 130 (0.001%) | **32.42 µs** | 71 ms | **322 ms** | **397 ms** |
| 150,000 (earlier 4-worker build) | 6,931,842 | 130 (0.002%) | 36.85 µs | 6 ms | 129 ms | 162 ms |
| 135,000 (cache-free baseline) | 6,244,233 | 105 (0.002%) | 38.44 µs | 1 ms | 122 ms | 158 ms |

The 200,000-user hold passes all thresholds (`p95 < 500 ms`, `p99 < 1000 ms`,
HTTP errors `< 1%`). Total percentiles for that run were p95 316 ms and
p99 392 ms; connection failures were 10 (0.0001%), also below 1%.
The earlier 150k and 135k rows were measured before the C hot-path helpers and
are included for context only.

## What changed from the first merged submission

- The server uses **4 io_uring workers** instead of 8 when the challenge fd
  limit is 65,535. The original worker count was derived from a large local
  `ulimit -n`, which did not match the official environment.
- Removed every `sqlite3_clear_bindings()` call. SQLite marks a statement
  `expired` when a bound value changes and the statement has an `expmask`;
  the next `step()` then reparses the SQL. Removing the clears stopped the hot
  GET-post query from being recompiled on almost every request.
- Added a small C hot-path helper (`src/fast.c`):
  - `fast_append_i64()` writes integers directly with a two-digit table.
  - `fast_append_json_string()` scans with SSE2 and preserves the exact JSON
    escaping rules for quotes, backslashes, control characters, and UTF-8.
- Read-only handlers no longer open an explicit SQLite transaction. All
  SQLite access is serialized under the server's database mutex, so reads are
  still consistent; only write batches start a transaction and group commit.
- Repeated likes are detected with a primary-key read before opening a write
  transaction. If the `(user_id, post_id)` row already exists, the server
  returns `200` with `already_liked: true` without touching the write path.
- SQLite tuning: 256 MB page cache, `cache_spill=OFF`, 1 GB mmap, WAL with
  `synchronous=NORMAL`, and a manual WAL checkpoint every 8,192 frames.
  Writes remain durable in the WAL before their responses are sent.

## Architecture

- One `io_uring` ring per worker, multishot accept and multishot recv,
  provided-buffer rings, fixed file descriptors, and pooled response buffers.
- One SQLite connection and one global database mutex. CQEs from all workers
  are processed serially, which keeps SQLite operations single-threaded and
  gives a natural group-commit boundary at the end of each CQE batch.
- The feed is assembled from three SQLite queries on every request: the latest
  20 post IDs, the likes in that ID range, and the matching post/user rows.
  There is no feed cache.
- JWT HS256 signatures are verified with OpenSSL on every authenticated
  request; no token verification is remembered across requests.

## Build

```bash
sudo bash install.sh          # installs the pinned Cider toolchain
bash build.sh                 # builds SQLite, Cider source, and src/fast.c
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
PORT=8080 bash test/run.sh submissions/cider-iouring-pomilon-v2
```

The suite covers all five endpoints, authentication failures, validation,
special-character round-trips, the 500-character post limit, likes, and feed
ordering. It passed **42/42** locally and **42/42** on the cloud server.
