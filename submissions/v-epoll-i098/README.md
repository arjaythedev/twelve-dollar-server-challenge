# V + raw epoll

| | |
|---|---|
| Language | V 0.5.2 (commit `7647ce1`), built from source by `install.sh` from its pinned C bootstrap (vlang/vc `7eb8c54`) |
| Framework | none: a single-threaded HTTP/1.1 server on Linux `epoll`, about 1,300 lines in `main.v` |
| SQLite driver | the SQLite C API called directly from V, SQLite 3.54.0 amalgamation (SHA-256 checked) compiled by `build.sh` |
| JWT | HMAC-SHA256 from OpenSSL's libcrypto (Ubuntu `libssl-dev`), JSON claims parsed with V's `x.json2` |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## Running it

```bash
sudo bash install.sh   # apt packages, then V 0.5.2 from source into /opt/v (a few minutes)
bash build.sh          # downloads and compiles SQLite 3.54.0, then builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **One event loop, one SQLite connection for requests.** The box has one vCPU. One thread owns the
  socket table and runs every query, so there are no mutexes, no thread hand-offs and no lock waits
  between requests. SQLite is compiled with `SQLITE_THREADSAFE=2` (each connection stays on its thread).
- **Group commit per `epoll_wait` batch.** All requests that arrive in one batch run in one transaction.
  Responses are held until `COMMIT` succeeds, so a 201 always means the row is committed (rule 6).
  If the commit fails, the batch's connections are closed instead of answered.
  WAL with `synchronous=NORMAL`.
- **WAL checkpoints on a second thread, never on the event loop.** SQLite's auto-checkpoint runs inside
  `COMMIT` and does two `fsync`s, which stalls every connection on a single loop (8 ms to 4 s each on a
  busy disk). Here a `sqlite3_wal_hook` only records the WAL size; at 1,000 frames (SQLite's default) a
  checkpointer thread with its own connection runs a PASSIVE checkpoint and reports through an `eventfd`
  in the same epoll set. If frames were appended meanwhile, write requests are held for at most 20 ms
  (reads go on) so one small follow-up checkpoint can let the WAL restart. No data is kept in the process:
  only the checkpoint timing moves.
- **Feed in three small queries.** The newest 20 ids come from the `(created_at, id)` index. When those
  ids span fewer than 64 ids (they do, because ids grow with time), one range scan reads the posts and
  authors, and one range scan on `likes_post_id_idx` counts the likes. Else the reference query runs.
  Every value comes from SQLite during the request (rule 5).
- **Prepared statements** for every query, plus `BEGIN`/`COMMIT`, made once at startup.
- **JSON written by hand into reused buffers.** The writer skips 8 bytes at a time when no byte needs
  escaping. Keys come out in the spec order.
- **Every token is verified.** HS256 only, signature compared in constant time, then `exp`, `sub` and
  `username` are checked on each request.
- **Many users on 65,535 descriptors.** In direct mode every k6 user holds its own keep-alive connection.
  Connections are kept in a least-recently-active list. When the descriptor budget is used up, the
  longest-idle connection with no work in flight is closed, and a new one is accepted. That client
  reconnects on its next request. This keeps `accept` working past 65k users instead of failing with `EMFILE`.
- **No GC scan of the event array.** The `epoll_data` union is declared with its `fd` member only.
  With a pointer member, V's Boehm GC support scans the 1,024-entry event array in every loop pass.
- **Edge-triggered sockets, one write per connection per batch**, `TCP_NODELAY`, partial writes kept
  per connection until the socket drains, and `EPOLLOUT` registered only while output is pending.
- **Idle connections stay open** with no timeout, so keep-alive works for Nginx or k6.

Nothing on the box is tuned: the process only raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local load test

Unchanged `bench/load.js` (k6 v2.3.0) with the README protocol, server in a Docker `ubuntu:24.04` container
(`--cpus=1 --memory=2g --memory-swap=2g --ulimit nofile=65535:65535`, direct mode), k6 on other cores of a shared
x86_64 host. Not the droplet score.

| Build | Users (5-min hold) | p95 | p99 | Errors | Peak RSS |
|---|---:|---:|---:|---:|---:|
| this submission | **81,250** | 498 ms | 695 ms | 0.00% | 125 MiB |

`bash test/run.sh submissions/v-epoll-i098`: 42 passed, 0 failed.
