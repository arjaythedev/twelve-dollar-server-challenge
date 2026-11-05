# Rust io_uring feed server

**Language:** Rust 1.94.0, `io-uring` 0.7.15, libc.
**Transport:** raw Linux io_uring with direct file descriptors. `SHARDS` independent listener/rings, all polled by one event-loop thread.
**Driver:** vendored SQLite 3.46.1 amalgamation, called directly through the C API and SQLite's internal B-tree cursors. No `rusqlite`.
**Serving:** direct on `HOST:PORT`; no Nginx.

## Architecture

- One event-loop thread owns HTTP parsing, JWT verification, SQLite reads and writes, and all ring I/O. There are no reader or writer threads; `SHARDS` only controls the number of io_uring rings/listeners, not concurrency.
- Each ring has its own io_uring, `SO_REUSEPORT` listener, fixed direct-descriptor table (65k slots), and provided-buffer ring. Accepted sockets use io_uring direct descriptors, so they do not consume the process `RLIMIT_NOFILE` budget.
- A pass drains completions from every ring, executes the collected writes in one `BEGIN IMMEDIATE`/`COMMIT`, refreshes the B-tree cursors, then resolves all reads and releases responses. Responses to writes are only queued after the commit succeeds.
- Writes use one transaction per pass and `json_each` batch inserts with `RETURNING`, so posts and likes are not prepared/executed one request at a time. A like is an `INSERT ... ON CONFLICT DO NOTHING`; the post-existence check only runs for like jobs that produced no row.
- `/feed` and `/posts/:id` use SQLite's own B-tree cursors, not the SQL VM. Feed requests collected in one pass share one read transaction and one response packet; batched post reads likewise share one cursor batch.
- Like counts come from a direct walk over `likes_post_id_idx`, not `SELECT count(*)`: dense post-id ranges (the feed) are walked once, while large gaps (scattered post ids collected in one pass) reseek and continue.
- SQLite runs in WAL mode with `synchronous=NORMAL`. No mmap for reads; the SQLite page cache holds parsed B-tree pages. Feed and post requests are read while they are served; there is no cross-request response cache.

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | Database path |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `SHARDS` | `8` | io_uring rings/listeners polled by the single event loop |
| `MULTISHOT` | `1` | `0` disables multishot recv |
| `STATS` | unset | `1` enables latency counters |
| `WRITE_DEFER_US` | `0` | Defer write batches for this many microseconds; `0` disables |
| `WRITE_BATCH` | `64` | Flush deferred writes once this many jobs are queued |
| `READ_CACHE_MB` | `16` | SQLite page cache for the read connection |
| `READ_MMAP_MB` | `256` | SQLite mmap window for the read connection |

## Build and run

```bash
bash install.sh   # Rust 1.94.0, build-essential
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

## Verification

```bash
bash test/run.sh submissions/rust-iouring-rkkautsar
```

The submission passes all 42 checks on a fresh seed database. On host networking with the 1.5 GiB test cap: 160k connections measured p50 26 ms, p95 103–107 ms, p99 165–185 ms; 200k with 8 rings measured p50 22 ms, p95 158–177 ms, p99 341–376 ms; 230k with 8 rings and 10 scale clients measured p50 46 ms, p95 158 ms, p99 219 ms. 280k was tested at 9–12 rings and did not pass the p95 gate (best: p50 110 ms, p95 694 ms, p99 965 ms at 10 rings/10 clients); the remaining limit is kernel TCP/netfilter CPU on the 1 vCPU box, not SQLite. Each client stayed under 0.002% failed requests.
