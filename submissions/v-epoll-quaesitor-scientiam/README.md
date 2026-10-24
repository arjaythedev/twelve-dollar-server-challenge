# V + raw epoll (+ fasthttp elsewhere)

| | |
|---|---|
| Language | V 0.5.2, master `b5bcdbb224` (2026-10-23), built from source by `install.sh` into `/opt/twelve-v` |
| Framework | none on Linux: a single-threaded HTTP/1.1 server on `epoll` (`src/backend_epoll_linux.c.v`). On macOS/Windows, or with `-d use_fasthttp`, vlib's `fasthttp` reactor serves the same handlers |
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

## Design

- **One thread, one epoll set, one SQLite connection.** The box has one vCPU; nothing is shared
  between threads, so there are no locks on the request path.
- **One transaction per `epoll_wait` batch.** Every request of a batch runs inside one deferred
  transaction. `COMMIT` runs once, and only then are the batch's responses sent, so a 201 always
  means the row is on disk (rule 6). If the commit fails, the connections that wrote are closed
  without an answer instead of being told a lie. WAL, `synchronous=NORMAL`.
- **64 bytes per idle connection.** A connection record holds three (pointer, len, cap) buffer
  handles and four counters. Input is read into one 64 KiB scratch buffer per loop; only an
  incomplete request tail, or a response the socket did not take whole, is copied into a
  per-connection buffer, which goes back to a small pool once drained.
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
- **Backpressure**: a connection whose response did not fit stops being read until `EPOLLOUT`
  drains it; connections stalled mid-request or mid-response for 120 s are closed; idle keep-alive
  connections are never timed out. When the descriptor budget is full the listener is taken out of
  the epoll set until a connection closes.
- Nothing on the box is tuned: the process only raises its own soft `RLIMIT_NOFILE` to the hard
  limit.

`SERVER_PROCS=n` (not used by `start.sh`) starts the listener with `SO_REUSEPORT` and normal
SQLite locking so several processes can share the port.

## Tests and local measurements

`bash test/run.sh submissions/v-epoll-quaesitor-scientiam`: 42 passed, 0 failed (Ubuntu 24.04 /
epoll with the PGO build, and macOS / fasthttp).

`tests/bench.sh` runs the server in a 1 CPU / 2 GB `ubuntu:24.04` container (Docker Desktop,
arm64) with k6 in a sibling container driving `tests/hold.js`, and reports the app's cgroup CPU
time per request. Not the droplet score; use it for A/B only:

| Build | req/s | app µs/req (user) | p95 | p99 | errors |
|---|---:|---:|---:|---:|---:|
| reference feed query | 3,340 | 61 (—) | 0.68 ms | 1.29 ms | 0 |
| three-scan feed | 3,340 | 50 (—) | 0.51 ms | 1.00 ms | 0 |
| + PGO, SWAR escaper | 3,340 | 43–47 (20–24) | 0.45 ms | 0.95 ms | 0 |

`bench/load.js` at 10,000 VUs for 2 minutes: 880 req/s, p95 0.50 ms, p99 0.73 ms, 0 failed
requests, 60 MB. On the same rig `v-epoll-i098` measures 46–48 µs/req, so the two are at
parity per request; about half of each request is kernel time (one `epoll_wait`, `recv` and
`send` per request).
