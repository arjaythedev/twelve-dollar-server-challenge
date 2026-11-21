# c-epoll-festoinc

A single-threaded `epoll` + SQLite implementation of the $12 Server Challenge API.

| | |
|---|---|
| **Language / compiler** | C (GCC 13/14, `-O3 -march=x86-64-v2 -flto -fno-plt`) |
| **Framework** | none: hand-written HTTP/1.1, JSON, JWT (HS256), base64url |
| **SQLite** | 3.54.1 amalgamation, included in the server translation unit (`SQLITE_THREADSAFE=0`) |
| **Reads** | fixed plans on SQLite's own B-tree cursors (`sqlite3Btree*`), prepared SQL fallback |
| **Other dependencies** | none (SHA-256/HMAC and JSON are in `server.c`) |
| **Nginx or direct** | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the challenge box) |
| **Tests** | 42/42 pass with `bash test/run.sh submissions/c-epoll-festoinc` |

## Running it

```bash
sudo bash install.sh          # apt: build-essential, curl, unzip
bash build.sh                 # downloads pinned SQLite, builds bin/server
SQLITE_PATH=/path/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

Configuration is exactly the challenge contract:

| Variable | Default | Notes |
|---|---|---|
| `SQLITE_PATH` | `feed.db` | the fresh copy of `seed/feed.db` |
| `JWT_SECRET` | `twelve-dollar-challenge` | HS256 key |
| `HOST` / `PORT` | `127.0.0.1` / `3000` | `0.0.0.0` / `80` when serving direct |
| `WORKERS` | 4 direct, 1 on loopback | worker processes sharing the port with `SO_REUSEPORT` |
| `DEBUG_CKPT` | unset | set to 1 to log WAL checkpoint stats |

`WORKERS` matters because the challenge gives each process `LimitNOFILE=65535`.
One process can hold ~60k keep-alive connections. When serving direct, the default
is four workers (about 240k connections); when running behind Nginx (which keeps
only 64 connections) the default is one worker.

## Design

- **One event loop per worker process.** The challenge box has one vCPU; each
  worker is a single thread with one `epoll` loop. Workers run under
  `SCHED_BATCH` by default: with several runnable processes on one CPU, longer
  time slices and less wakeup preemption measurably improve total throughput
  (`NO_SCHED_BATCH=1` opts out). Every query is an indexed
  lookup that takes microseconds, so SQLite is called inline on the loop.
- **Hand-written HTTP/1.1** with keep-alive and pipelined requests. Idle
  connections own no user-space buffers: there is one shared 64 KB read buffer,
  and a connection only gets its own buffer while it holds a partial request or a
  response the socket could not take yet. This is what lets a process hold tens
  of thousands of keep-alive clients in a 2 GB box.
- **B-tree read fast path.** The amalgamation is `#include`d in `server.c`, so
  SQLite's internal B-tree cursor API is visible. `/feed` and `GET /posts/:id`
  run fixed plans directly on those cursors: scan
  `posts_created_at_id_idx` for the newest ids, walk the `posts` table backwards
  when the ids are consecutive, seek `users` by rowid, and count likes with one
  range scan of `likes_post_id_idx`. Every page is still read through SQLite's
  pager/WAL/page cache inside a read transaction (rule 5); only the SQL bytecode
  interpreter is skipped. Any surprise (overflowing record, missing row, a wide
  id span, unexpected schema) falls back to the prepared SQL statement, so the
  fast path cannot change semantics.
- **Prepared statements for writes and fallback**, with
  `SQLITE_PREPARE_PERSISTENT`. WAL + `synchronous=NORMAL`, a 1 GiB `mmap_size`,
  a 16 MB page cache.
- **Durable writes before the response.** Each write runs in a short
  `BEGIN IMMEDIATE … COMMIT`; the 201/200 response is queued only after the
  commit returns. Different requests in one event-loop batch still share the
  transaction, but the lock is never held across the reads of the batch.
- **Checkpoints off the request path.** A helper process forked before the
  database is opened runs `wal_checkpoint(PASSIVE)` and adapts its sleep to the
  WAL size (1 ms when the log is large, 50 ms when it is small). The serving
  processes keep `wal_autocheckpoint` at 50,000 pages as a safety net. On a
  1-vCPU box this keeps the WAL at a few MB and keeps checkpoint fsyncs out of
  the p99.
- **No caching across requests (rule 5).** Every request runs its SQL and every
  token's HMAC is computed and compared. The only precomputed value is the
  HMAC key schedule derived from `JWT_SECRET`; it does not depend on a request.
- **Fast response path.** Responses are built into a 16 MB arena, HTTP headers
  are formatted without `printf`, and JSON strings take an SSE2 fast path that
  `memcpy`s the common no-escape case.

## Correctness details

- Auth is checked before the post id on `/posts/:id/like`, as specified.
- JWT: exactly three base64url parts, HS256 only, signature verified with
  HMAC-SHA256, `exp` checked, then `sub` must be a positive integer string and
  `username` must be a string.
- JSON request bodies are parsed with a small recursive-descent parser (escapes,
  `\uXXXX` and surrogate pairs included), trimmed, and length-checked by UTF-8
  code points.
- `created_at` is returned exactly as stored; SQLite's column default supplies
  the new value.
- The golden feed/post response bytes match `test/golden/*.json` exactly.

## Local benchmark notes

Measured on a 4-vCPU Xeon Silver 4314 development machine with the server pinned
to one CPU (`taskset -c 0`) and k6 on the other three. These are not the
challenge box's numbers, but they are useful for tuning:

The included `bench_c.c` is a small epoll load generator that follows the same
mix without think time, so it can actually saturate the server (k6 on three
cores tops out around 10–13k req/s and becomes the bottleneck). With the server
pinned to **one** CPU and `bench_c` on the other three:

| Workload | Request rate | App CPU | µs/req |
|---|---:|---:|---:|
| mixed mix, 200 connections | **~37k req/s** | ~95% of one core | 25.8 |
| mixed mix, 5,000 connections | **~36k req/s** | ~99% of one core | 27 |
| mixed mix, random likes (more inserts), 5,000 conns | **~30–32k req/s** | ~95% of one core | 30 |
| mixed mix, 4 workers (direct mode), 5,000 conns | **~29–30k req/s** | ~98% of one core | 33 |
| mixed mix, 2 workers, 5,000 conns | **~32k req/s** | ~98% of one core | 31 |
| `GET /health` | ~13k req/s | — | 23 µs/req |
| `GET /feed` (B-tree) | ~11.5k req/s | — | 42 µs/req |
| `GET /posts/:id` (B-tree) | ~11.3k req/s | — | 35 µs/req |

The WAL stays at ~1–11 MB depending on the write mix, because the checkpoint
helper adapts to the log size. At the challenge's user model (~2.2 requests per
iteration and ~20 s of think time per iteration), 30k req/s is roughly 300k
users, which is the top of the leaderboard.

For comparison, before the B-tree fast path the same machine did ~10–13k req/s
at 40–70 µs/req. The fast path cut `/feed` from ~73 to ~42 µs and the mixed
load from ~44 to ~26 µs per request.

The development box is shared (load average ~2 on 4 vCPUs), so repeated runs
vary by roughly 20%. Best observed: **36.9k req/s with one worker** and
**30.3k req/s with four workers** (`SCHED_BATCH`, 5,000 connections, random
likes). A dedicated 1-vCPU benchmark box should be steadier than this.

## Deploying on a 2 GB VPS

See [`deploy/README.md`](deploy/README.md) for a step-by-step systemd deployment
and a k6 load-test recipe. The short version:

```bash
# build and install the service (as root, once)
sudo bash install.sh
bash build.sh
sudo useradd --system --home /opt/feed --shell /usr/sbin/nologin deploy || true
sudo mkdir -p /opt/feed/bin
sudo cp bin/server /opt/feed/bin/
sudo cp ../../seed/feed.db /opt/feed/feed.db
sudo cp deploy/feed-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now feed-server
```

## What is next

The read path is now at the leaderboard's per-request cost. The remaining
overheads are networking (one `recv`/`send` pair and epoll bookkeeping per
request) and the multi-worker context switches needed to get past the
65,535-descriptor per-process cap. The next steps, in order of expected gain,
are:

1. Edge-triggered epoll with read/write loops (fewer `epoll_wait`/`epoll_ctl`
   calls), or io_uring multishot accept/recv/send as the top entries use.
2. A profile-guided build (the reference submissions see 5–10%).
3. Batching the read transaction across an event-loop pass to amortise
   `sqlite3BtreeBeginTrans`/`Commit`.
4. Tuning the worker count and socket buffer sizes for 300k idle connections on
   the 2 GB box.
