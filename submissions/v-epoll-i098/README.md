# V + io_uring fixed files

| | |
|---|---|
| Language | V 0.5.2 (commit `7647ce1`), built from source by `install.sh` from its pinned C bootstrap (vlang/vc `7eb8c54`) |
| Framework | none: one V event loop with a C io_uring transport |
| SQLite driver | the SQLite C API called directly from V, SQLite 3.54.0 amalgamation (SHA-256 checked) compiled by `build.sh` |
| JWT | HMAC-SHA256 from OpenSSL's libcrypto (Ubuntu `libssl-dev`), JSON claims parsed with V's `x.json2` |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Network dependency | liburing 2.9, pinned to `08468cc3830185c75f9e7edefd88aa01e5c2f8ab`, statically linked |

## Running it

```bash
sudo bash install.sh   # apt packages, then V 0.5.2 from source into /opt/v (a few minutes)
bash build.sh          # builds pinned SQLite and liburing, then bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The transport requires Linux 6.0 or newer and an allowed `io_uring_setup` syscall.
Keep the process descriptor limit at 65,535 or higher.
Provide the same locked-memory allowance for this server and the Rust control.
The C transport keeps debug symbols and frame pointers for local profiles.

## Optimizations, and why

- **One event loop, one SQLite connection for requests.** The box has one vCPU. One thread owns the
  socket table and runs every query, so there are no mutexes, no thread hand-offs and no lock waits
  between requests. SQLite is compiled with `SQLITE_THREADSAFE=2` (each connection stays on its thread).
- **Group commit per completion batch.** Completions collect for at most 200 microseconds before
  one SQLite transaction. Responses enter the transport only after `COMMIT` succeeds.
  This also holds read responses that observe a write from the same batch.
  A failed commit discards the tentative responses and closes their connections.
  After a commit that wrote to the WAL, new write requests wait 5 ms (reads go on), so later writes
  share one commit. Each commit rewrites the same hot pages, so this cuts WAL frames and checkpoints.
  SQLite retains WAL mode with `synchronous=NORMAL`.
- **WAL checkpoints on a second thread, never on the event loop.** SQLite's auto-checkpoint runs inside
  `COMMIT` and does two `fsync`s, which stalls every connection on a single loop (8 ms to 4 s each on a
  busy disk). Here a `sqlite3_wal_hook` only records the WAL size; at 1,000 frames (SQLite's default) a
  checkpointer thread with its own connection runs a PASSIVE checkpoint and reports through an `eventfd`
  watched by a one-shot ring poll. Checkpoint completion wakes an otherwise idle server.
  A follow-up checkpoint can pause writes for 20 ms, or until completion above 16,000 WAL frames.
  It runs whenever writes appended frames during the first checkpoint, because the WAL can restart
  only when every frame is copied. Without it, steady writes can grow the WAL without a limit.
  The existing request connection and background checkpoint connection keep separate thread owners.
- **Feed in three small queries.** The newest 20 ids come from the `(created_at, id)` index. When those
  ids span fewer than 64 ids (they do, because ids grow with time), one range scan reads the posts and
  authors, and one range scan on `likes_post_id_idx` counts the likes. Else the reference query runs.
  Every value comes from SQLite during the request (rule 5).
- **Prepared statements** for every query, plus `BEGIN`/`COMMIT`, made once at startup.
- **JSON written by hand into reused buffers.** The writer skips 8 bytes at a time when no byte needs
  escaping. Keys come out in the spec order.
- **Every token is verified.** HS256 only, signature compared in constant time, then `exp`, `sub` and
  `username` are checked on each request.
- **524,280 fixed slots.** One thread owns eight sparse tables with 65,535 slots each.
  Direct accepts never install client sockets in the process descriptor table.
  The listener supplies `TCP_NODELAY` to accepted sockets.
  Each slot has at most one outstanding accept, send or close and a generation-tagged completion.
  The allocator cannot reuse a slot until the server consumes its direct-close completion.
- **One multishot receive per connection.** A request costs no receive submission or poll re-arm.
  Input that arrives while V is still busy with the connection is held, and the receive is cancelled
  until V asks for input again, so pipelined clients keep backpressure in their socket.
  A server close cancels the armed receive first, then closes the direct descriptor.
- **Shared receive buffers.** Each ring has 1,024 buffers of 2,048 bytes.
  Idle connections have no input or output allocation.
  Partial requests use bounded input buffers. Pending sends retain stable storage until completion.
  The transport delivers input directly after a send unless V has buffered or deferred work to process.
- **Bounded queues, no SQPOLL.** Rings use single-issuer cooperative task execution. The server
  submits within the SQ limit and consumes bounded CQ batches.
  `NODROP` protects overflow completions. `ENOBUFS` retries wait for buffer recycling.
  Resource failures delay retries. Full fixed tables pause accepts without closing idle clients.
  The loop blocks on ring descriptors, including the ring that reports checkpoint completion.
- **Idle connections stay open** with no timeout, so keep-alive works for Nginx or k6.

Nothing on the box is tuned: the process only raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Earlier epoll local load test

Unchanged `bench/load.js` (k6 v2.3.0) with the README protocol, server in a Docker `ubuntu:24.04` container
(`--cpus=1 --memory=2g --memory-swap=2g --ulimit nofile=65535:65535`, direct mode), k6 on other cores of a shared
x86_64 host. Not the droplet score.

These historical measurements do not measure the fixed-file implementation above.

| Build | Users (5-min hold) | p95 | p99 | Errors | Peak RSS |
|---|---:|---:|---:|---:|---:|
| earlier epoll build | **81,250** | 498 ms | 695 ms | 0.00% | 125 MiB |

`bash test/run.sh submissions/v-epoll-i098`: 42 passed, 0 failed.
