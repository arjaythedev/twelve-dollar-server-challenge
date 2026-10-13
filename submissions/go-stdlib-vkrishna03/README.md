# Go, standard library only

| | |
|---|---|
| Language | Go 1.27.2 (official tarball, checksum verified in `install.sh`) |
| Server | One epoll event loop and a small HTTP/1.1 parser, Go standard library only |
| SQLite | 3.45.1, Ubuntu 24.04's `libsqlite3-dev` |
| SQLite binding | A thin cgo layer (`sqlite.go`), no `database/sql` |
| Dependencies | None beyond the Go standard library and SQLite (no `go.sum`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, libsqlite3-dev; Go 1.27.2 into /usr/local/go
bash build.sh          # bin/server
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

**SQLite through a thin cgo layer** (`sqlite.go`, ~140 lines):

- Statements are prepared once (`SQLITE_PREPARE_PERSISTENT`) and reset right after each use, so no
  read transaction outlives its request. That caches query plans, not data (rule 5).
- One cgo call steps a row *and* reads all its columns into a C struct, instead of one call per
  column. Text comes back as zero-copy views into SQLite's memory and is escaped straight into the
  response buffer. A feed costs 21 cgo calls and allocates nothing.
- `locking_mode=EXCLUSIVE`: this process is the database's only user, so the WAL index lives in
  heap memory instead of the `-shm` file, and queries take no file locks. **17% less CPU per
  request** on the mixed workload.
- `SQLITE_CONFIG_MULTITHREAD` and `SQLITE_CONFIG_MEMSTATUS=0` drop SQLite's per-connection and
  memory-statistics mutexes.
- Pragmas: WAL, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 16 MiB page cache,
  `temp_store=MEMORY`. SQLite's default auto-checkpoint (1000 pages) runs on the loop. A checkpoint
  only copies pages that are already committed.
- SQL: the reference queries; their plans are already index-only lookups. A like is one statement
  (`INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`). A post-existence check runs
  only when nothing was inserted, to tell "already liked" (200) from "no such post" (404).
- Writes autocommit. `INSERT ... RETURNING` is stepped to `SQLITE_DONE`, which commits it, before
  a 201 is written.

**JWT verified on every request** (rule 5): HS256 only, signature checked with `hmac.Equal` using
a reused HMAC, `exp` and `nbf` checked. `sub` must be a positive integer string and
`username` a string. The write path (~8% of requests) uses `encoding/json` for the token and the
request body. It is strict and simple, and fast enough there.

**Small things:** the `Date` header is formatted once per second, response bodies are built in
reused buffers, and `GOGC=400` because the heap stays at a few MB.

**Tried and rejected:**

- Building the JSON in SQL (`json_object` + `group_concat`). Measured **43% more CPU** per feed
  than one-call-per-row plus Go encoding.
- Multiple event loops with `SO_REUSEPORT`, and a separate connection that checkpoints off the
  loop. Exclusive locking is faster and needs a single connection, and the box has one vCPU.

## Measurements

Local, not the droplet: Ubuntu 24.04 arm64 in Docker on an Apple M3. The server container was
pinned to one core with 2 GB RAM, no swap and `nofile=65535`. The load came from separate
containers on the other cores, over Docker's bridge network.

Max throughput with no think time (wrk, 64 connections):

| Workload | req/s | Server CPU per request |
|---|---:|---:|
| `GET /feed` | 44,900 | 22.6 µs |
| `GET /posts/:id` | 279,000 | 3.6 µs |
| Mix of `bench/load.js` (100 feed : 100 post : 15 like : 2 create) | 76,200 | 11.2 µs |

`bench/load.js` with k6 1.8.2, 10,000 VUs (30 s ramp, 60 s hold): 103,642 requests, **0 errors**,
p95 1.6 ms, p99 10.5 ms, ~9% CPU, 35 MB.

k6 needs ~280 KB per VU, so higher counts used a small load generator that runs the same user loop
and think times (users split over several containers so no source address runs out of ports):

| Users | Hold | Requests | Errors | p95 | p99 | Server CPU | Server memory |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 20,000 | 60 s | 168,593 | 0 | 1.7 ms | 22 ms | ~25% | ~70 MB |
| 60,000 | 5 min | 2,123,544 | 0 | 28 ms | 190 ms | ~42% (half of it kernel) | ~75 MB |
| 65,000 | 90 s | 859,530 | 0 | 11 ms | 31 ms | ~45% | ~70 MB |

Tail latency on this setup is noisy: identical 60k runs gave p99 between 31 and 206 ms. With a
server instrumented to log slow work, no request took over 5 ms and no loop pass over 10 ms. The
tail comes from waiting for the CPU, not from the handlers or SQLite.

At 66,000 users, about 470 users got no descriptor and timed out (0.07% of requests). The server
kept serving everyone else at the same latency and CPU.

## Correctness

`test/run.sh` passes 42/42. A separate raw-socket suite (not part of this folder) checks the cases
above: pipelining, 7-byte packets, output backpressure, `100-continue`, HTTP/1.0, oversized and
conflicting headers, 405s, trailing garbage and non-object JSON bodies, JavaScript-whitespace
trimming, the 500-character limit with multi-byte characters, control-character escaping
round-trips, JWT `nbf`/`alg`/claim types, pipelined duplicate likes, 500 concurrent connections, and
keep-alive surviving 66 s idle.

## License

MIT, under the repo's [license](../../LICENSE).
