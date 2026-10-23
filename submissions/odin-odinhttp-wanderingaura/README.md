# Odin + odin-http + SQLite

| | |
|---|---|
| Language | Odin `dev-2026-09` |
| HTTP | [odin-http](https://github.com/laytan/odin-http) `fac113f` (io_uring via `core:nbio`) with a small memory patch ([`patches/odin-http-pool.patch`](patches/odin-http-pool.patch)) |
| SQLite | 3.53.4 amalgamation, built by `build.sh`, hand-written bindings ([`sqlite.odin`](sqlite.odin)) |
| Nginx | **No, direct**: serves `HOST:PORT` itself |

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
- **Memory allocated once.** Connection structs, buffers, request blocks and SQLite's page cache
  come from huge-page slabs allocated and touched at startup, so serving never page-faults or
  grows the heap.
- **SQLite tuned for one connection.** Statements are prepared once and reused.
  `locking_mode=EXCLUSIVE` removes the shared-memory file and per-transaction locking. WAL with
  `synchronous=NORMAL`, `mmap`, and a library built single-threaded (no mutexes) with
  `-O3 -march=native`. Database pages are faulted in before the server starts listening.
- **Page cache dropped at startup.** Installing and building leaves about 1 GB of package,
  toolchain and build files in the page cache, which on a 2 GB box pushes the kernel into reclaim
  at high connection counts. Before opening the database, the server walks `/usr`, `/var`, `/etc`
  and its own directory and calls `posix_fadvise(DONTNEED)` on every file it can read
  ([`pagecache.odin`](pagecache.odin)). It needs no root and changes no kernel setting; only
  clean, unmapped pages are dropped, and the database and its WAL are skipped. It takes about
  0.3 s for 10k files. In a 2 GB container it took 100k connections from failing (p99 1.35 s) to
  passing (p99 25 ms).
- **No intermediate objects.** Column text is copied straight from SQLite into a preallocated
  output buffer, with 16-byte SIMD JSON escaping.

Every request still reads SQLite (no response caches), and every JWT signature is verified.
Writes are autocommit, so each one is committed before its 201.

## Results (local)

Clean Ubuntu 24.04 container limited to 1 CPU and 2 GB, load generated on the same machine:

| Users | Load | Failed | p95 | p99 | Server CPU | Memory |
|---:|---|---:|---:|---:|---:|---:|
| 60,000 | k6, `bench/load.js` | 0 of 717,922 | 0.9 ms | 17 ms | 15% | 304 MB RSS |
| 100,000 | C generator\* | 0 of 634,700 | 11 ms | 25 ms | 22% | 847 MB container |
| 200,000 | C generator\* | 0 of 1,268,914 | 21 ms | 31 ms | 38% | 1,421 MB container |

On the real box the service's 65,535 file descriptors cap it at about 65k users, one connection
each; CPU is not the limit. The 100k and 200k runs used a raised descriptor limit to see how far
memory goes: 200k connections (21k req/s) still fit in the 2 GB container.

\* The C load generator from `fortran-microhttpd-tbsvttr/tests/connection_load.c`: the same user
loop and think times as `bench/load.js`, light enough to simulate 100k+ users on one machine.
