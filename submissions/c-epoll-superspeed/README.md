# c-epoll-superspeed

A single-threaded `epoll` + SQLite implementation of the $12 Server Challenge API.

| | |
|---|---|
| **Language / compiler** | C (GCC 13/14, `-O3 -march=x86-64-v2 -flto -fno-plt`) |
| **Framework** | none: hand-written HTTP/1.1, JSON, JWT (HS256), base64url |
| **SQLite** | 3.54.1 amalgamation, compiled into the binary (`SQLITE_THREADSAFE=0`) |
| **Other dependencies** | none (SHA-256/HMAC and JSON are in `server.c`) |
| **Nginx or direct** | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the challenge box) |
| **Tests** | 42/42 pass with `bash test/run.sh submissions/c-epoll-superspeed` |

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
  worker is a single thread with one `epoll` loop. Every query is an indexed
  lookup that takes microseconds, so SQLite is called inline on the loop.
- **Hand-written HTTP/1.1** with keep-alive and pipelined requests. Idle
  connections own no user-space buffers: there is one shared 64 KB read buffer,
  and a connection only gets its own buffer while it holds a partial request or a
  response the socket could not take yet. This is what lets a process hold tens
  of thousands of keep-alive clients in a 2 GB box.
- **Prepared statements, persistent preparation.** The reference queries are
  used, with `SQLITE_PREPARE_PERSISTENT`. WAL + `synchronous=NORMAL`, a 1 GiB
  `mmap_size`, a 16 MB page cache.
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

| Workload | Request rate | p99 | App CPU |
|---|---:|---:|---:|
| mixed `bench/load.js` mix, no think time, 500 VUs | **~10–13k req/s** | 110–160 ms | ~1 core |
| `GET /health` | ~13k req/s | — | 20 µs/req |
| `GET /feed` | ~10k req/s | — | 70 µs/req |
| `GET /posts/:id` | ~10k req/s | — | 42 µs/req |
| `POST /posts/:id/like` | ~10k req/s | — | 45 µs/req |

At the challenge's user model (~2.2 requests per iteration and ~20 s of think
time per iteration), 10k req/s is roughly 100k–130k users. The WAL stays at
~2–3 MB under the mixed load.

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

The current cost is dominated by SQLite's SQL VM (roughly 38 µs for the 20-row
`/feed` query on a warm cache). The top of the leaderboard reads the SQLite
B-tree pages directly (still through SQLite's page cache and mmap, which rule 5
allows) and gets to ~26 µs/request. Adding a B-tree cursor fast path for
`/feed`, `/posts/:id` and like counts, and a profile-guided build, are the
obvious next steps if we want to push past the current ~10–13k req/s.
