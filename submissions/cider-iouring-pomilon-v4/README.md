# Twelve Dollar Server Challenge — Cider io_uring v4 (global batch + off-loop checkpoint)

A Cider HTTP server built around `io_uring`, SQLite 3.54.1, and OpenSSL.
The application, event loop, HTTP/JSON logic, authentication, and SQL are
written in Cider. Two small C leaf helpers are kept exactly as in v3:

- `src/btree.c` includes the pinned SQLite amalgamation and implements the
  native SQLite B-tree read plans. SQLite still owns the pager, WAL,
  transactions, locking, and durability.
- `src/fast.c` provides the SSE2 JSON string escape and integer formatter.

There is no C server/framework wrapper, no external service, and no
cross-request response or query cache. Every request reads or writes SQLite
while it is being served.

## Architecture

This submission ports the execution architecture of the C freestanding v2
server into Cider:

- **One event-loop thread drives all five `io_uring` rings.** There is no
  worker thread per ring. Each ring has an eventfd registered with
  `IORING_REGISTER_EVENTFD`; all eventfds share one epoll set. The loop sleeps
  in `epoll_wait()` instead of polling.
- **One global batch per loop.** CQEs from every ring are decoded first into a
  bounded job arena. The request jobs then run in one SQLite transaction with
  all reads first and all writes second. Exactly one `bt_end_batch()` and one
  `COMMIT` happen per batch, and responses are flushed to the rings only after
  the commit succeeds.
- **Held connections.** A write (or a full job arena) holds its connection
  until the batch has committed and its responses have been released; the
  remaining pipelined bytes are then parsed.
- **Off-loop WAL checkpoint.** SQLite is built with `SQLITE_THREADSAFE=2`.
  A dedicated pthread owns a second SQLite connection and runs
  `sqlite3_wal_checkpoint_v2(PASSIVE)` when the WAL hook reports roughly 1000
  frames. If the WAL passes 64 MiB and the background thread is not already
  working, the event loop may checkpoint once between batches.
- **Bounded memory.** Five rings × 65,535 fixed descriptor slots, 1,024
  provided receive buffers per ring, a 4 MiB SQLite page cache
  (`cache_size=-4096`), and no retained response-buffer pool — response
  buffers are freed after sending.
- **JWT key schedule only.** The HMAC-SHA256 key pads are computed once at
  startup. Every request still verifies its own signature and claims; no
  authentication result is cached.

At the official `LimitNOFILE=65535`, `target_slots=300000` produces five
rings. The process uses three threads: the main thread (waits on the event
thread), the event-loop thread, and the checkpoint thread.

## What changed from Cider v3

- Per-ring worker threads replaced by one event-loop thread, epoll, and
  eventfd.
- Per-ring CQE handling changed from decode-and-serve to decode-only; all
  request jobs execute in one global read-first/write-second batch.
- One `COMMIT` and one `bt_end_batch()` per batch instead of per worker.
- Off-loop PASSIVE WAL checkpoint on a second SQLite connection.
- `SQLITE_THREADSAFE=2`, `locking_mode=NORMAL`, `busy_timeout=5000`,
  `cache_size=-4096`.
- 1,024 provided buffers per ring instead of 4,096.
- Response-buffer pool removed.
- HMAC-SHA256 key pads precomputed at startup.

The wire behavior, HTTP/JSON parsing, authentication, B-tree read plans, and
SQL statements are unchanged from v3.

## Local verification

- Build succeeds with the pinned Cider v3 compiler.
- `TMPDIR=/home/pomilon/tmp PORT=3108 bash test/run.sh submissions/cider-iouring-pomilon-v4`
  passes **42/42**, both with the default descriptor limit (8 rings) and with
  `ulimit -n 65535` (5 rings).
- At `LimitNOFILE=65535` the process reports **3 threads** and **5 io_uring
  fds**, and the checkpoint thread wakes and runs PASSIVE checkpoints during a
  400-post write burst (WAL stayed around 4 MiB).
- A concurrent smoke test (2,000 parallel feed reads, 200 parallel post
  creates, 100 parallel duplicate likes) returned only the expected
  `200`/`201` statuses with no server errors.

## Cloud benchmark

Synteq 1 vCPU / 2 GB server, 4 vCPU / 4 GB load generator, five source
IPv4 addresses, fresh seed DB, 42/42 tests, 120 s ramp / 300 s hold:

| users | requests | HTTP failures | CPU/request | hold p50 | hold p95 | hold p99 |
|---:|---:|---:|---:|---:|---:|---:|
| **290,000** | 13,438,680 | 183 (0.001%) | **25.75 µs** | 2 ms | **45 ms** | **120 ms** |
| **280,000** | 12,976,687 | 153 (0.001%) | **25.92 µs** | 1 ms | **5 ms** | **20 ms** |

The 290k hold passes p95 < 500 ms, p99 < 1 s, and errors < 1%. Total
percentiles were p95 42 / p99 127 ms; connection failures were 1,028
(0.0076%). Peak RSS was 668 MB. The 280k run had 11 connection failures,
648 MB RSS, and total p95 6 / p99 41 ms.

## Build

```bash
sudo bash install.sh          # installs the pinned Cider toolchain
bash build.sh                 # builds SQLite + B-tree plans, Cider source, helpers
```

`build.sh` pins the SQLite amalgamation version and checksum and compiles
SQLite with `SQLITE_THREADSAFE=2`. For local builds, `vendor` may be a symlink
to an existing submission's `vendor` directory; it is ignored by git and must
not be committed.

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
PORT=8080 bash test/run.sh submissions/cider-iouring-pomilon-v4
```
