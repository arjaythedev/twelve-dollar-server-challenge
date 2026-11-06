# Rust io_uring feed server

**Language:** Rust 1.94.0, `io-uring` 0.7.15, libc.
**Transport:** raw Linux io_uring with direct file descriptors. `SHARDS` independent listeners/rings, all polled by one event-loop thread.
**Driver:** vendored SQLite 3.46.1 amalgamation, called directly through the C API and SQLite B-tree cursors. No `rusqlite`.
**Serving:** direct on `HOST:PORT`; no Nginx.

## Architecture

- One event-loop thread owns HTTP parsing, JWT verification, SQLite reads and writes, and all ring I/O. `SHARDS` controls the number of io_uring rings/listeners, not thread count.
- Each ring has its own io_uring, `SO_REUSEPORT` listener, direct-descriptor slots, and (with the default multishot recv) a provided-buffer ring. Accepted sockets use io_uring direct descriptors, so they do not consume the process `RLIMIT_NOFILE` budget.
- A pass drains completions from every ring, executes collected writes in one `BEGIN IMMEDIATE`/`COMMIT`, refreshes B-tree cursors, then resolves all reads and releases responses. Write responses are queued only after the commit succeeds.
- Each `/feed` and `/posts/:id` request executes its own B-tree read while it is served. There is no cross-request read or response coalescing and no response/query-result cache.
- Writes use one transaction per pass and `json_each` batch inserts with `RETURNING`. A like is an `INSERT ... ON CONFLICT DO NOTHING`; post-existence is checked only for like jobs that produced no row.
- `/feed` and `/posts/:id` use SQLite B-tree cursors, not the SQL VM. Like counts come from a direct walk over `likes_post_id_idx`: dense post-id ranges are walked once, while large gaps reseek.
- Writer prepared statements are cached across batches; parameters are rebound and statements reset for each batch.
- io_uring completions are processed directly, without a per-ring staging vector.
- SQLite runs in WAL mode with `synchronous=NORMAL`. Reads use the SQLite page cache (`READ_CACHE_MB`) and its mmap window (`READ_MMAP_MB`; set `0` to disable mmap).

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | Database path |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `SHARDS` | `8` | io_uring rings/listeners polled by the single event loop |
| `MULTISHOT` | `1` | `0` disables multishot recv |
| `STATS` | unset | `1` enables latency counters |
| `PASS_STATS` | unset | Any value prints per-100-pass timing to stderr |
| `WRITE_DEFER_US` | `0` | Defer write batches for this many microseconds; `0` disables |
| `WRITE_BATCH` | `64` | Flush deferred writes once this many jobs are queued |
| `READ_CACHE_MB` | `16` | SQLite page cache for the read connection |
| `READ_MMAP_MB` | `0` | SQLite mmap window for the read connection; `0` disables |

## Build and run

```bash
bash install.sh   # Rust 1.94.0, build-essential
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` verifies the pinned Rust toolchain tarball with SHA-256 before installing it.

## Verification

```bash
bash test/run.sh submissions/rust-iouring-rkkautsar
```

The submission passes all 42 checks on a fresh seed database. Every request reads its own data from SQLite while served; there is no cross-request coalescing or response cache.

At 280k virtual users (23,333 connections, 12 independent loadgen processes, server pinned to one CPU), all 12 loadgen processes passed: p95 ≤0.6 ms, p99 ≤1.9 ms, max ≤117 ms, ~2.3k requests/s, and no network errors.
