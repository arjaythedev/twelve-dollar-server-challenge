# V + io_uring fixed files

| | |
|---|---|
| Language | V 0.5.2 (commit `7647ce1`), built by `install.sh` with one `cc -O2` of its pinned C bootstrap (vlang/vc `7eb8c54`, `vc/v.c`; no self-rebuild) |
| Framework | none: one V event loop with a C io_uring transport |
| SQLite driver | the SQLite C API called directly from V, SQLite 3.54.0 amalgamation (SHA-256 checked) compiled by `build.sh` |
| JWT | HMAC-SHA256 from OpenSSL's libcrypto (Ubuntu `libssl-dev`); JWT claims and post bodies are parsed by hand in `main.v` |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Network dependency | liburing 2.9, pinned to `08468cc3830185c75f9e7edefd88aa01e5c2f8ab`, statically linked |

## Running it

```bash
sudo bash install.sh   # apt packages, then V 0.5.2 into /opt/v (about 3 minutes on 1 vCPU)
bash build.sh          # builds pinned SQLite and liburing, then bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The transport requires Linux 6.1 or newer (deferred task work) and an allowed `io_uring_setup` syscall.
Keep the process descriptor limit at 65,535 or higher.
Provide the same locked-memory allowance for this server and the Rust control.
The C transport keeps debug symbols and frame pointers for local profiles.

## Optimizations, and why

- **One event loop, one SQLite connection for requests.** The box has one vCPU. One thread owns the
  socket table and runs every query, so there are no mutexes, no thread hand-offs and no lock waits
  between requests. SQLite is compiled with `SQLITE_THREADSAFE=2` (each connection stays on its thread)
  and the other options from SQLite's recommended compile-time list that the server does not need.
- **Group commit per completion batch.** Completions collect for at most 4 milliseconds before
  one SQLite transaction. Responses enter the transport only after `COMMIT` succeeds.
  This also holds read responses that observe a write from the same batch.
  A failed commit discards the tentative responses and closes their connections.
  After a commit that wrote to the WAL, new write requests wait 5 ms (reads go on), so later writes
  share one commit. Each commit rewrites the same hot pages, so this cuts WAL frames and checkpoints.
  SQLite retains WAL mode with `synchronous=NORMAL`.
  With a partial batch, the loop sleeps to its deadline instead of waking for each completion.
- **WAL checkpoints on a second thread, never on the event loop.** SQLite's auto-checkpoint runs inside
  `COMMIT` and does two `fsync`s, which stalls every connection on a single loop (8 ms to 4 s each on a
  busy disk). Here a `sqlite3_wal_hook` only records the WAL size; at 256 frames a
  checkpointer thread with its own connection runs a PASSIVE checkpoint and reports through an `eventfd`.
  This starts writeback earlier than SQLite's default 1,000-frame interval.
  A one-shot ring poll watches the `eventfd`; checkpoint completion wakes an otherwise idle server.
  A follow-up checkpoint can pause writes for 20 ms, or until completion above 16,000 WAL frames.
  It runs whenever writes appended frames during the first checkpoint, because the WAL can restart
  only when every frame is copied. At 16,000 frames, writes wait in either phase until the running
  checkpoint ends, so a slow disk cannot grow the WAL (and its dirty pages) without a limit.
  The checkpoint thread is a plain pthread, not a V thread: the garbage collector stops the world
  by signalling every thread it knows, and a thread stuck in `fsync` cannot answer, so the process
  aborted with "Signals delivery fails constantly" when the disk stalled.
  The existing request connection and background checkpoint connection keep separate thread owners.
- **Interpolated row seeks.** `td_sqlite.c` finds a post or author by rowid with the same page walk as
  `sqlite3BtreeTableMoveto`, but the first probe on each page interpolates between its first and last key
  (ids are dense) and a miss gallops before it bisects. It reads the same B-tree pages on every request and
  keeps nothing between requests. 65.1k to 63.4k instructions per request.
- **SQLite page cache, no mmap.** Pages come from a 64 MiB page cache (`cache_size=-65536`), which holds
  the hot pages of this workload. With `mmap_size` set, each page fetch costs SQLite more instructions:
  turning it off cut server instructions per request by 3.8% in a local profile.
- **Feed in three small queries.** The newest 20 ids come from the `(created_at, id)` index. When those
  ids span fewer than 64 ids (they do, because ids grow with time), one range scan reads the posts and
  authors, and one range scan on `likes_post_id_idx` counts the likes. The post scan runs newest id
  first and writes each row straight into the response; any other order runs the reference query.
  Every value comes from SQLite during the request (rule 5).
- **Feed and post reads use SQLite's B-tree layer.** `/feed` and `/posts/:id` read through SQLite's own
  B-tree cursors, in `td_sqlite.c`, which `build.sh` compiles in the same translation unit as the pinned
  amalgamation. They read the same database file in the request's read transaction, so they see the same
  snapshot as SQL. There is no result cache: between requests the code keeps only root page numbers and
  key descriptions. Unusual cases (overflow pages, wide id spans, unknown ids) run the SQL path.
- **Prepared statements** for every query, plus `BEGIN`/`COMMIT`, made once at startup.
- **JSON written by hand into reused buffers.** A feed or post row is written by one C function
  (`row_out.h`) straight into the response buffer. It copies each string 16 bytes at a time (SSE2) while
  it checks for a byte that needs escaping, and finishes that string bytewise only after the first such
  byte. Keys come out in the spec order. `tests/row_out.c` compares it with a plain reference writer.
- **Every token is verified.** HS256 only, signature compared in constant time, then `exp`, `sub` and
  `username` are checked on each request.
- **524,280 fixed slots.** One thread owns eight sparse tables with 65,535 slots each.
  Direct accepts never install client sockets in the process descriptor table.
  The listener supplies `TCP_NODELAY` to accepted sockets.
  It also caps receive and send buffers at 4 KiB and 8 KiB before Linux doubles them for accounting.
  Each slot has at most one outstanding accept, send or close and a generation-tagged completion.
  The allocator cannot reuse a slot until the server consumes its direct-close completion.
- **One multishot receive per connection.** A request costs no receive submission or poll re-arm.
  Input that arrives while V is still busy with the connection is held, and the receive is cancelled
  until V asks for input again, so pipelined clients keep backpressure in their socket.
  A server close cancels the armed receive first, then closes the direct descriptor.
  A reset also notifies V while application work is deferred, so dead sockets do not wait for a checkpoint.
  A client that only closes its write side can still receive its response.
- **Shared receive buffers.** Each ring has 1,024 buffers of 2,048 bytes.
  Idle connections have no input or output allocation.
  Partial requests use bounded input buffers. Pending sends retain exact-size storage until completion.
  A delayed checkpoint can release many short replies at once; they must not each reserve a full feed-sized block.
  The transport delivers input directly after a send unless V has buffered or deferred work to process.
- **Bounded queues, no SQPOLL.** Rings use single-issuer cooperative task execution. The server
  submits within the SQ limit and consumes bounded CQ batches.
  `NODROP` protects overflow completions. `ENOBUFS` retries wait for buffer recycling.
  Resource failures delay retries. Full fixed tables pause accepts without closing idle clients.
  The loop blocks on ring descriptors, including the ring that reports checkpoint completion.
- **Idle connections stay open** with no timeout, so keep-alive works for Nginx or k6.

Nothing on the box is tuned: the process only raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local load tests

Local only, not a benchmark-box score. The server ran in a Docker container with `--cpus=1 --cpuset-cpus=<one core>
--memory=2g --memory-swap=2g --ulimit nofile=65535:65535 --ulimit memlock=-1:-1 --security-opt seccomp=unconfined`
(io_uring needs the last two in Docker), direct mode, on a shared 96-core x86_64 host, fresh seed copy each run.

| Build | Load | Users (5-min hold) | p95 | p99 | Errors | Server CPU per request | Peak memory |
|---|---|---:|---:|---:|---:|---:|---:|
| earlier io_uring build, run 1 | native C++ port of `bench/load.js`, 8 cores | **270,000** | 24.6 ms | 46.6 ms | 0.00% | 32.1 µs | 1,351 MiB |
| earlier io_uring build, run 2 | same | **270,000** | 52.5 ms | 124.7 ms | 0.00% | 32.9 µs | 1,406 MiB |
| earlier epoll build | `bench/load.js` on k6 v2.3.0 | 81,250 | 498 ms | 695 ms | 0.00% | - | 125 MiB RSS |

The native generator keeps the `bench/load.js` user model (feed, think, post, think, 15% like, 2% post),
because local k6 was killed at its 40 GB memory limit while holding 120,000 users. Both 270,000-user runs
passed `PRAGMA integrity_check`, and every acknowledged post and like was in the database afterwards.
These holds predate the response-allocation, socket-buffer, and reset-cleanup changes.

`bash test/run.sh submissions/v-epoll-i098`: 42 passed, 0 failed.

The transport regression checks reset cleanup during deferred work and preserves responses after input half-close:

```sh
cc -O2 -Wall -Wextra -Werror -I build/liburing/src/include tests/transport_reset.c \
  build/liburing/src/liburing.a -o build/transport-reset-test
sudo prlimit --nofile=65535:65535 --memlock=unlimited:unlimited build/transport-reset-test
```
