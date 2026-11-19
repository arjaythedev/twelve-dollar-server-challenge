# Odin + odin-http + SQLite

| | |
|---|---|
| Language | Odin `dev-2026-09` |
| HTTP | [odin-http](https://github.com/laytan/odin-http) `fac113f` (io_uring via `core:nbio`) with a small memory patch ([`patches/odin-http-pool.patch`](patches/odin-http-pool.patch)) |
| SQLite | 3.53.4 amalgamation, built by `build.sh` with PGO, hand-written bindings ([`sqlite.odin`](sqlite.odin)) |
| Nginx | **No, direct**: serves `HOST:PORT` itself |
| Architectures | x86-64; `install.sh` also picks Odin's arm64 build, nothing is tuned for arm64 |

```bash
sudo bash install.sh && bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Main optimisations

- **Direct, single thread.** No Nginx (it would take CPU and add two socket hops per request).
  One io_uring event loop batches every send and receive into one syscall per loop pass.
- **Cheap connections.** Upstream odin-http gives each connection its own virtual arena,
  which caps the server at about 32k connections (Ubuntu's `vm.max_map_count`). The patch lends
  a request memory from a shared pool only while it's being served, so an idle keep-alive
  connection costs almost nothing. It also closes immediately when the client has already gone,
  instead of after a 500 ms delay: with thousands of disconnects at once (the end of every k6
  run), the delayed closes queued behind the io_uring ring and stalled new connections. After
  60k simultaneous disconnects the server now answers again in 0.6 s instead of 7.8 s.
- **Connections retired at the descriptor limit.** Keep-alive is normal until open connections
  reach the descriptor limit minus 4,096 (61,439 under `LimitNOFILE=65535`). Above that, each
  response says `connection: close` and its connection is closed once it's sent: the user who was
  just served is the one least likely to send again soon (3–15 s of think time), and one close per
  response spreads the closes out. The socket closes at once unless the client sent bytes we never
  read (checked with `FIONREAD`), which still get the 500 ms linger. 32 accepts stay in flight so
  reconnecting users are taken in batches. Holding every connection open instead would cost
  1.8 GB at 250k users; churn costs about 1.3 µs per request.
- **Memory allocated once.** Connection structs, buffers, request blocks and SQLite's page cache
  come from huge-page slabs allocated and touched at startup, so serving never page-faults or
  grows the heap.
- **SQLite tuned for one connection.** Statements are prepared once and reused.
  `locking_mode=EXCLUSIVE` removes the shared-memory file and per-transaction locking. WAL with
  `synchronous=NORMAL`, `mmap`, and a library built single-threaded (no mutexes) with
  `-O3 -march=native`. Database pages are faulted in before the server starts listening.
- **Feed as range scans.** The reference feed query does 60 B-tree descents (row, author and like
  count for each of 20 posts) and was 56% of user CPU. The server reads the 20 newest ids from the
  `(created_at, id)` index; when they are consecutive (newest posts nearly always are), it reads the
  rows and the like counts as two scans over that id range, 23 descents in all. Otherwise it runs
  the reference query. At startup both paths must produce the same bytes or the server exits.
  14% less CPU per request. The queries are taken from [cpp-eastl-io_uring-pgo-hendy643]; the
  range-scan idea is also in [rust-epoll-scooter1337].
- **SQLite PGO.** `build.sh` builds SQLite instrumented, runs the server's own statements in the
  load test's mix against a synthetic database ([`pgo.odin`](pgo.odin), never the seed), then
  rebuilds SQLite with that profile. 3% less CPU per request. Both of those submissions do this
  too.
- **Page cache dropped at startup.** Installing and building leaves about 1 GB of package,
  toolchain and build files in the page cache, which on a 2 GB box pushes the kernel into reclaim
  at high connection counts. Before opening the database, the server walks `/usr`, `/var`, `/etc`
  and its own directory and calls `posix_fadvise(DONTNEED)` on every file it can read
  ([`pagecache.odin`](pagecache.odin)). It needs no root and changes no kernel setting; only
  clean, unmapped pages are dropped, and the database and its WAL are skipped. It takes about
  0.3 s for 10k files.
  passing (p99 25 ms).
- **No intermediate objects.** Column text is copied straight from SQLite into a preallocated
  output buffer, with 16-byte SIMD JSON escaping.

Every request still reads SQLite (no response caches), and every JWT signature is verified.
Writes are autocommit, so each one is committed before its 201.

## Results (local)

Clean Ubuntu 24.04 container built with `install.sh` and `build.sh`, limited to 1 CPU, 2 GB and
`nofile=65535`, in its own network namespace. The load came from the C generator\* running in that
namespace from a separate, unlimited container, in 50k-user shards. 60 s ramp, then a 120 s hold
(not the scored 5 minutes); latency is the worst shard's. The host is a Zen 5 Threadripper, whose
core is faster than a droplet vCPU, so expect fewer users on the real box.

| Users | Requests/s | Failed | p95 | p99 | Server CPU (peak) | Container memory |
|---:|---:|---:|---:|---:|---:|---:|
| 250,000 | 26,500 | 0 | 4.2 ms | 5.2 ms | 44% | 510 MiB |
| 550,000 | 58,100 | 0 | 41 ms | 63 ms | 87% | 549 MiB |
| 575,000 | 60,600 | 0 | 71 ms | 81 ms | 99% | 551 MiB |
| 600,000 | — | 1.3% | 281 ms | 13 s | 99% | 549 MiB |

CPU is the limit; descriptors stay at the 61.5k cap and memory stays flat.

\* The C load generator from `fortran-microhttpd-tbsvttr/tests/connection_load.c`: the same user
loop and think times as `bench/load.js`, light enough to simulate 100k+ users on one machine.

The main bottlenecks are sqlite & the odin-http library. Even though odin-http has been patched to be faster for this workload, hand rolling an io_uring loop would likely be faster.