# C + io_uring

| | |
|---|---|
| Language | C (GNU C11), Ubuntu 24.04's default GCC, `-O3 -march=native -flto`, PGO |
| Framework | none: a hand-written single-threaded-per-worker `io_uring` server (Linux 6.0+, `liburing`), with an `epoll` fallback for older kernels |
| Database | SQLite 3.54.0 amalgamation built from source with PGO, `SQLITE_THREADSAFE=0` |
| JSON / JWT | hand-written scanner, writer, HMAC-SHA256 (pads hashed once at startup), base64url |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

liburing 2.15 and the SQLite amalgamation are pinned by SHA-256 in `build.sh` and built from
source on the box. No prebuilt binaries. If any PGO step fails, the build falls back to a plain
`-O3` build; if the kernel lacks multishot io_uring, the server falls back to epoll at startup.
Both fallbacks are logged on stderr.

## Design

- **One event loop per worker, up to 8 workers.** Each worker holds its own io_uring with a
  registered file table of up to 65,535 direct-descriptor slots, so accepted sockets do not
  consume process file descriptors and one process can hold far more than `LimitNOFILE`
  keep-alive connections. Workers take turns on one mutex around the database, so on one
  vCPU they never compete.
- **Multishot accept and receive.** Accepts land straight in the file table; receives draw
  from a provided-buffer ring (2,048 x 2 KiB per worker), so an idle connection holds no
  buffer at all, only its 88-byte connection record.
- **Wake-ups are batched.** Under load the loop sleeps until 128 completions are ready or
  4 ms pass, whichever comes first. One wake-up then parses, queries, commits and submits
  many responses at once.
- **Group commit.** The first write of a loop batch opens `BEGIN IMMEDIATE`; every response
  produced while it is open, reads included, waits for the commit before it is sent. If the
  commit fails, those responses are replaced by a 500 and the connection is closed, so a
  201 is always a committed row (rule 6).
- **The feed as three range scans.** The 20 newest post ids come from the `created_at, id`
  index; if their span is small, one scan counts all their likes and one scan of `posts`
  (joined to `users`) fetches their rows, then they are written out in index order. If the
  ids are spread out, the reference query runs instead. Every response still comes from
  queries made for that request (rule 5).
- **SQLite** runs inline on the worker thread with `locking_mode=EXCLUSIVE` (one process,
  one connection: read transactions do no `fcntl` lock calls), WAL, `synchronous=NORMAL`,
  a 128 MiB page cache and `wal_autocheckpoint=0`; a passive checkpoint runs after commits
  once the WAL grows past a few thousand frames, off the response path.
- **JWT checked on every authenticated request**: HS256 only, signature compared in
  constant time, `exp` (and `nbf` if present) checked with sub-second precision, and the
  payload re-validated (`sub` a positive integer, `username` a string). The HMAC pads are
  hashed once at startup, so a verification costs two short hash continuations.
- **No allocation on the request path.** Requests are parsed in place in the receive
  buffers; responses are built into one-page chunks from a per-worker arena, with the
  header written in front of the body and sent with one `send`. Freed chunks are reused
  cache-hot first.
- **Memory guardrails.** Usable memory (MemAvailable minus reclaimable slab and CMA) is
  checked four times a second: below 192 MB new connections are not accepted; below 96 MB
  idle keep-alives are shed until it recovers.
- **Backpressure and stalls.** A connection with more than 64 KiB of unsent responses stops
  being read until half is out. A partial request or unsent response that makes no progress
  for 30 s closes the connection. Accepted sockets inherit `TCP_NODELAY` and TCP keepalives.
- **epoll fallback** (kernels before 5.19, or where io_uring is disabled): the same request
  path on level-triggered epoll with `EPOLLEXCLUSIVE` accepts, on-demand buffers, and the
  same batching, group commit and guardrails.

## Build

`build.sh` downloads and checksum-verifies liburing 2.15 and SQLite 3.54.0, builds both
with profile-guided optimization (a synthetic database and a synthetic request mix drive the
instrumented builds; no challenge seed data is used), then builds `bin/server`. The PGO
trainer and the load generator live in `pgo/` and are not linked into the server. A smoke
test at the end starts the binary and requires a healthy `/health`.

`--epoll` forces the fallback; `--pgo <seconds>` runs for that long and exits cleanly (used
by the build's training step).

## Tests

`test/test.sh`: 42/42. Unit vectors for SHA-256/HMAC/base64url/JSON are exercised during
development via `src/selftest.c` (not part of the build).

## Local numbers

2-core cloud VM, epoll fallback (kernel 5.10), k6 on the other core, PGO build, fresh seed
copy. The load generator sharing the box is the limiting factor here, not the server; at
12,000 users the box's usable memory drops below the shedding threshold because k6 itself
needs ~3.4 GB. On the droplet the load comes from separate machines.

| Users | p95 | p99 | Failed |
|---:|---:|---:|---:|
| 8,000 | 1.1 ms | 4.5 ms | 0.00% |
| 10,000 | 1.2 ms | 4.9 ms | 0.00% |

## License

MIT, under the repo's license.
