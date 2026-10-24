# V + raw io_uring (epoll and fasthttp as fallbacks)

| | |
|---|---|
| Language | V 0.5.2, master `b5bcdbb224` (2026-10-23), built from source by `install.sh` into `/opt/twelve-v` |
| Framework | none on Linux: an HTTP/1.1 server on `io_uring` (`src/backend_uring_d_uring.c.v`, liburing behind a small C shim), one ring per thread. The same binary falls back to a single-threaded `epoll` loop (`src/backend_epoll_linux.c.v`) when io_uring is unavailable. On macOS/Windows, or with `-d use_fasthttp`, vlib's `fasthttp` reactor serves the same handlers |
| SQLite | the C API called directly, SQLite 3.53.4 amalgamation (SHA-256 checked) compiled by `build.sh` |
| JWT | HS256 with vlib `crypto.hmac`/`crypto.sha256`; claims read by a small validating JSON reader |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## Running it

```bash
sudo bash install.sh   # apt packages, then the pinned V compiler from source (a few minutes)
bash build.sh          # downloads and compiles SQLite 3.53.4, then builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`VBIN=/path/to/v bash build.sh` builds with another compiler (local development).
`URING=0 bash build.sh` builds the epoll-only binary. At run time `RINGS=n` sets the number of
io_uring threads (default: enough rings of `RLIMIT_NOFILE` slots each to hold 8 × 65,535
connections, at most 8). Under Docker, io_uring needs `--security-opt seccomp=unconfined`;
without it the server prints why and runs the epoll loop.

## Design

- **io_uring, one ring per thread, direct descriptors.** Each ring registers a sparse file table
  of `RLIMIT_NOFILE` slots (the kernel's per-ring maximum), so accepted sockets never occupy
  process descriptors and 8 rings hold up to 8 × 65,535 connections under the spec's
  `LimitNOFILE=65535`. Multishot accept and multishot receive into 4,096 kernel-provided 2 KiB
  buffers per ring; a ring stops accepting when fewer than 1,024 of its slots are free, so the
  others take over. Completions are handled in batches of up to 32, waiting at most 250 µs for
  them while busy.
- **One SQLite connection, exclusive locking, shared under one mutex.** A ring takes the lock,
  handles its completion batch, commits, submits the responses, and releases it. The box has one
  vCPU, so the rings mostly take turns anyway, and the database never sees two writers.
- **One transaction per completion batch.** Every request of a batch runs inside one deferred
  transaction. `COMMIT` runs once, and only then are the batch's responses sent, so a 201 always
  means the row is committed (rule 6; WAL with `synchronous=NORMAL`, so it survives a server
  crash, and the next checkpoint makes it survive a power loss). If the commit fails, or SQLite
  rolled the transaction back on its own, none of the batch's responses is sent and its
  connections are closed instead of being told a lie.
- **epoll fallback.** If `io_uring_setup` fails (an old kernel, a seccomp sandbox), the same
  binary runs its single-threaded epoll loop: one `epoll_wait` batch per transaction, the same
  handlers and the same connection records.
- **64 bytes per idle connection.** A connection record holds three (pointer, len, cap) buffer
  handles and four counters. Requests are parsed in place from the kernel-provided buffer
  (io_uring) or a 64 KiB scratch buffer (epoll); only an incomplete request tail, or a response
  the socket did not take whole, is copied into a per-connection buffer, which goes back to a
  small pool once drained.
- **Responses are appended, never allocated.** Handlers write status line, headers and JSON straight
  into the batch arena from SQLite's column pointers; integers and strings are formatted by hand in
  the spec's key order.
- **Every token is verified**: HS256 only, signature compared in constant time, then `exp`, `sub` and
  `username` are checked. Every request reads SQLite live; nothing is cached.
- **Feed in three scans.** The newest 20 ids come from the `(created_at, id)` index; when they span
  fewer than 256 ids, one range scan reads the posts and authors and one range scan on
  `likes_post_id_idx` counts the likes, and the rows are put back in feed order. Otherwise the
  reference query runs. Every value comes from SQLite during the request (18% less CPU per
  request locally). `FEED_RANGE=0` in the environment turns it off.
- **Checkpoints** run after a commit once the WAL holds 4,096 frames (`wal_autocheckpoint=0`).
- **Profile-guided build.** `build.sh` compiles SQLite and the server instrumented, lets the server
  serve a synthetic database to itself over loopback (`bin/server --train`; the challenge seed is
  never used), then rebuilds both with that profile. `bash build.sh --no-pgo` skips it.
- **Backpressure**: a connection whose responses are not being read is parked, that is no longer
  read, until its queue drains (epoll: wait for `EPOLLOUT`; io_uring: one send in flight, later
  responses queued behind it, and past 64 KiB of queue the receive is cancelled until the send
  completions catch up). Unparsed input is capped at 16 KiB per connection, queued output at
  4 MiB. Connections stalled mid-request or mid-response for 120 s are closed; idle
  keep-alive connections are never timed out. When a ring's slots run out it cancels its accept
  until a connection closes.
- Nothing on the box is tuned: the process only raises its own soft `RLIMIT_NOFILE` to the hard
  limit.

`SERVER_PROCS=n` in `start.sh` (default 1) runs n processes sharing the port (`SO_REUSEPORT`)
and the database (normal locking, autocommit per statement, busy timeout), with either
backend. It is the epoll build's only way past the `LimitNOFILE` ceiling and it is not free:
locally one process costs 47 µs/request, the same process without `locking_mode=EXCLUSIVE` 51,
and two processes 59. The io_uring build does not need it.

## Tests and local measurements

`bash test/run.sh submissions/v-epoll-quaesitor-scientiam`: 42 passed, 0 failed (Ubuntu 24.04
with the PGO io_uring build, Ubuntu 24.04 / epoll, and macOS / fasthttp).

`tests/bench.sh` runs the server in a 1 CPU / 2 GB `ubuntu:24.04` container (Docker Desktop,
arm64) with k6 in a sibling container driving `tests/hold.js`, and reports the app's cgroup CPU
time per request. Not the droplet score; use it for A/B only:

| Build | req/s | app µs/req (user) | p95 | p99 | errors |
|---|---:|---:|---:|---:|---:|
| epoll, reference feed query | 3,340 | 61 (—) | 0.68 ms | 1.29 ms | 0 |
| epoll, three-scan feed | 3,340 | 50 (—) | 0.51 ms | 1.00 ms | 0 |
| epoll + PGO, SWAR escaper | 3,340 | 43–47 (20–24) | 0.45 ms | 0.95 ms | 0 |
| **io_uring** + PGO, 8 rings | 3,340 | **31–32 (15–16)** | 0.56 ms | 0.95 ms | 0 |

`bench/load.js` at 10,000 VUs for 2 minutes (epoll build): 880 req/s, p95 0.50 ms, p99 0.73 ms,
0 failed requests, 60 MB. On the same rig `v-epoll-i098` measures 46–48 µs/req, the same as the
epoll build, of which about half is kernel time (one `epoll_wait`, `recv` and `send` per
request). io_uring removes a third of the total: the per-request syscalls become batched
submissions and multishot completions, and user time drops too because the request is parsed
in the kernel-provided buffer without a copy.
