# Rust io_uring feed server

**Language:** Rust 1.94.0, `io-uring` 0.7.15, libc.
**Transport:** raw Linux io_uring with direct file descriptors. `SHARDS` independent listener/ring/event-loop threads; one SQLite read worker per shard and one shared SQLite writer thread.
**Driver:** vendored SQLite 3.46.1 amalgamation, called directly through the C API and SQLite's internal B-tree cursors. No `rusqlite`.
**Serving:** direct on `HOST:PORT`; no Nginx.

## Architecture

- The event loop owns HTTP parsing, JWT verification, feed/post reads, and all ring I/O.
- Accepted sockets use io_uring direct descriptors in per-ring file tables, so they do not consume the process `RLIMIT_NOFILE` budget.
- Rings are dynamic: the server starts with one ring and one `SO_REUSEPORT` listener. When total active connections approach the direct-table capacity, it stops using that ring for new capacity and adds another ring plus listener. The same thread polls all rings.
- `/feed` and `/posts/:id` use SQLite's own B-tree cursors, not the SQL VM. Feed and post requests collected in one pass are answered by one read transaction and one shared response packet. `READ_COALESCE=0` disables the per-pass read batching.
- Writes are committed by one writer thread. The writer prepares `INSERT ... RETURNING` / optimistic like statements once per batch, runs one `BEGIN IMMEDIATE`/`COMMIT`, and only sends responses after `COMMIT`. A like is a direct `INSERT ... ON CONFLICT DO NOTHING`; the post-existence check runs only when the insert changed zero rows.
- Feed and post requests are read while they are served; there is no cross-request response or query-result cache.
- SQLite runs in WAL mode with `synchronous=NORMAL`. The writer batches write jobs (up to 2048 per transaction) and checkpoints the WAL off the event loop.
- No mmap for reads; the SQLite page cache holds parsed B-tree pages.

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | Database path |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `SHARDS` | `4` | Independent event-loop/listener shards; one read worker each |
| `READ_COALESCE` | `1` | `0` disables global feed/post batching |
| `COALESCE_US` | `10000` | In-flight feed/post coalescing window in microseconds |
| `WRITE_COALESCE_US` | `100000` | Write group-commit coalescing window in microseconds |

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

The submission passes all 42 checks on a fresh seed database.
