# Go, standard library only

| | |
|---|---|
| Language | Go 1.27.2 (official tarball, checksum verified in `install.sh`) |
| Server | One epoll event loop and a small HTTP/1.1 parser, Go standard library only |
| SQLite | 3.53.4, compiled in from the official amalgamation (`build.sh` downloads it and checks its sha256) |
| SQLite binding | A thin cgo layer (`sqlite.go`), no `database/sql` |
| Dependencies | None beyond the Go standard library and SQLite (no `go.sum`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl; Go 1.27.2 into /usr/local/go
bash build.sh          # downloads SQLite 3.53.4, builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` supports x86_64 (the droplet) and arm64.

## Design, and why

**One epoll event loop does everything.** It handles accept, read, parse, SQLite, JSON and write
inline, one request after another. On one vCPU, worker pools or a goroutine per connection only
add context switches and memory. Go's `net/http` holds a goroutine plus 8 KiB of buffers for every
connection, idle or not. Other submissions measured it at ~1 GB and ~98% CPU at 50k users. Here an
idle connection is a ~100-byte struct with no buffers: requests are parsed in place from one shared
64 KiB read buffer, and only a request that arrives in pieces gets its own copy.

- **Level-triggered epoll**, one `read` and one `write` per request. With level triggering there is
  no extra read just to see `EAGAIN`, and `TCP_NODELAY` sends each response right away.
- **The HTTP/1.1 parser** handles keep-alive, pipelining, requests split across packets,
  `Content-Length` bodies, `Expect: 100-continue`, HTTP/1.0 and `Connection: close`. A full socket
  buffer is handled by parking the rest of the response and waiting for `EPOLLOUT`. Chunked request
  bodies get a 501, oversized headers a 431, and garbage a 400. Idle connections are closed after
  120 s (the spec asks for at least 65 s).
- **File-descriptor limit.** Each user holds a connection, so `LimitNOFILE=65535` caps the box at
  ~65.5k users. When `accept` fails with `EMFILE`, the loop stops watching the listener until a
  connection closes. Otherwise the listener would stay readable and spin the loop at 100% CPU.

**SQLite: about half the server's CPU, so most of the tuning is here.**

- **Compiled in, not Ubuntu's library.** SQLite 3.53.4 is built with the recommended compile-time
  options: `THREADSAFE=0` (one thread uses it, so no mutexes at all), `DEFAULT_MEMSTATUS=0`, and
  the `OMIT_*` options for unused features. **About 20% less CPU** per query than Ubuntu's 3.45.1.
- **The feed as three range scans.** Ids and `created_at` are both assigned at insert, so the 20
  newest posts almost always have adjacent ids. Then three range scans replace the reference
  query's 60 lookups (20 each for the post, its author and its like count):
  1. the 20 ids from the `created_at` index
  2. posts with their authors in `[lo, hi]`
  3. like counts in `[lo, hi]` from `likes_post_id_idx`

  That's a third less CPU per feed. If the ids are more than 64 apart, or a post has no author
  row, the reference query answers instead. The three scans see the same data, because this
  connection is the only writer and nothing runs in between. The output is byte-identical; the
  golden tests check it.
- `locking_mode=EXCLUSIVE`: this process is the database's only user, so the WAL index lives in
  heap memory instead of the `-shm` file, and queries take no file locks. **17% less CPU per
  request** on the mixed workload.
- **Checkpoints every 10,000 WAL pages (40 MB) instead of 1,000.** A checkpoint copies each
  changed page once however often it changed, and fsyncs twice on the event loop. So fewer, larger
  checkpoints cost less CPU, and a like or post blocks the loop for half as long.
- **No mmap**, a 16 MiB page cache. A 1 GiB mmap measured 6% slower per request.
- Statements are prepared once (`SQLITE_PREPARE_PERSISTENT`) and reset right after each use, so no
  read transaction outlives its request. That caches query plans, not data (rule 5).
- One cgo call steps a row *and* reads all its columns into a C struct, instead of one call per
  column. Text comes back as zero-copy views into SQLite's memory and is escaped straight into the
  response buffer. Request handling allocates nothing.
- A like is one statement (`INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`). A
  post-existence check runs only when nothing was inserted, to tell "already liked" (200) from
  "no such post" (404).
- Writes autocommit with WAL and `synchronous=NORMAL` (rule 6). `INSERT ... RETURNING` is stepped
  to `SQLITE_DONE`, which commits it, before a 201 is written.

**JWT verified on every request** (rule 5): HS256 only, signature checked with `hmac.Equal` using
a reused HMAC, `exp` and `nbf` checked. `sub` must be a positive integer string and
`username` a string. The write path (~8% of requests) uses `encoding/json` for the token and the
request body. It is strict and simple, and fast enough there.

**Small things:** the `Date` header is formatted once per second, response bodies are built in
reused buffers, and `GOGC=400` because the heap stays at a few MB.

**Tried and rejected.** Each was measured as server CPU during a 60k-user hold; the noise is about
±2 points.

| Idea | Result |
|---|---|
| **io_uring** instead of epoll (multishot accept and recv, buffer rings, direct descriptors), written in pure Go | Passes every test, but **the same CPU as epoll**. The kernel's time goes to TCP work, not syscalls |
| Batching wake-ups (wait 1–4 ms to collect more events), with epoll and with io_uring | No CPU saved; median latency +4–7 ms |
| Two processes with `SO_REUSEPORT` (each process gets its own 65,535 descriptors) | Works, but +12% CPU (no exclusive locking, two loops) |
| Small socket buffers, `TCP_DEFER_ACCEPT` | Within noise |
| Building the JSON in SQL (`json_object` + `group_concat`) | 43% *more* CPU per feed than Go encoding |
| A background checkpointer connection | Needs shared locking, which costs more than inline checkpoints |

## Measurements

Local, not the droplet: Ubuntu 24.04 arm64 in Docker on an Apple M3. The server container was
pinned to one core with 2 GB RAM, no swap and `nofile=65535`. The load came from separate
containers on the other cores.

**Calibrating against the droplet.** The official run of `python-fastapi-cknutson12` on the
droplet was 12,000 users at 84% CPU. The same server under the same load used 49% of an M3 core
here. `go-gnet-Haris0059` reports 62,500 users at 79% on a droplet, and used about 50% here (estimated from its whole-run average). So a
droplet vCPU is about **0.6 of this core** for this workload. "Droplet mode" below throttles the
server to that.

Max throughput with no think time (wrk, 64 connections):

| Workload | req/s | Server CPU per request |
|---|---:|---:|
| `GET /feed` | 49,200 | 20.6 µs |
| `GET /posts/:id` | 288,800 | 3.5 µs |
| Mix of `bench/load.js` (100 feed : 100 post : 15 like : 2 create) | 125,900 | 7.5 µs |

`bench/load.js` with k6 1.8.2, 10,000 VUs: 0 errors, p95 1.6 ms, p99 10.5 ms.

Higher counts used a small load generator that runs the same user loop and think times. k6 needs
~280 KB per VU, so it couldn't reach these counts here.

| Users | Server | Errors | p95 | p99 | Server CPU |
|---:|---|---:|---:|---:|---:|
| 60,000 (90 s hold) | one full core | 0 | 2.3–2.7 ms | 18–30 ms | ~47% |
| 64,000 (90 s hold) | **droplet mode, 0.5 core** (harsher than the ~0.6 calibration) | 0 | 159–598 ms | 253–917 ms | 94–97% of the quota |

At 64,000 users in 0.5 core, the previous version of this server (Ubuntu SQLite, reference feed
query, 1,000-page checkpoints) failed twice: p95 1.3–2.4 s. This version passed one run and was
borderline in the other. Both ran out of CPU, so this is about where a droplet's limit is. Above
65,535 users, the descriptor limit decides.

## Correctness

`test/run.sh` passes 42/42 on x86_64 and arm64. A separate raw-socket suite (not part of this
folder) checks a further 58 cases:
- HTTP framing: pipelining, 7-byte packets, output backpressure, `100-continue`, HTTP/1.0,
  oversized and conflicting headers, 405s
- JSON bodies: trailing garbage, non-object bodies, JavaScript-whitespace trimming, the
  500-character limit with multi-byte characters, control-character escaping round-trips
- JWT: `nbf`, `alg` and claim types
- Concurrency: pipelined duplicate likes, 500 concurrent connections, keep-alive surviving 66 s idle

`PRAGMA integrity_check` is `ok` after 60k-user runs.

## License

MIT, under the repo's [license](../../LICENSE).
