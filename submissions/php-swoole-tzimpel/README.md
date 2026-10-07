# PHP + Swoole

| | |
|---|---|
| Language | PHP 8.3.6 (Ubuntu 24.04's `php8.3-cli` package), OPcache + tracing JIT |
| Server | [Swoole](https://github.com/swoole/swoole-src) 6.2.3, built from the pinned release tarball (sha256-checked) by `build.sh`, with a small memory patch (below) |
| Framework | none: one `onRequest` callback with a hand-written router |
| SQLite driver | PDO SQLite (`php8.3-sqlite3`), linked to Ubuntu's SQLite 3.45.1 |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: php8.3-cli, php8.3-dev, php8.3-sqlite3, php8.3-opcache, build tools
bash build.sh          # downloads + compiles Swoole into build/swoole.so (a few minutes on 1 vCPU)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Architecture

```
k6 ── keep-alive connections ──► Swoole worker (1 process, SWOOLE_BASE, one epoll loop)
                                   router → JWT check → prepared statement → JSON bytes
                                   writes: group commit at the end of each loop iteration
                                        │
                              feed.db (WAL) ◄── checkpointer process (own connection,
                                                PASSIVE checkpoint every second)
```

## Optimizations, and why

Each one was measured as CPU time per request at a fixed 6,000 req/s request mix (the same
feed/post/like/create ratio as `bench/load.js`), with the server in a container limited to 1 CPU and 2 GB.
Variants were run interleaved, because the host's speed drifted by about 10% between runs.

- **Swoole in `SWOOLE_BASE` mode, one worker, no coroutines.** HTTP parsing, keep-alive handling and
  the event loop are C. BASE mode means the worker accepts and answers connections itself, with no
  reactor-to-worker IPC hop. Every handler is synchronous, so `enable_coroutine` is off and no
  coroutine is created per request. SQLite is called inline: every query is an index lookup of a few
  microseconds, so a thread pool would only add hops on one core.
- **Direct instead of Nginx.** Nginx's `worker_connections 16384` counts client *and* upstream sockets,
  so it caps the test at about 16k users, and it costs 14–20% of the only CPU. Swoole holds 40,000
  keep-alive connections without trouble (see the numbers below). There is no heartbeat, so idle
  connections are never closed by the server.
- **Patch: free idle receive buffers.** Out of the box, Swoole keeps a fresh 64 KiB receive buffer,
  allocated from PHP's heap, on every keep-alive connection after it answers a request. With 40,000
  connections that is 2.5 GB counted against `memory_limit`, and the worker died at about 38k
  connections. Even without a limit it is about 16 KiB of resident memory per connection.
  `build.sh` patches the two "request done" branches in `src/server/port.cc` to free the buffer instead.
  - Before: 10k used connections took 184 MB RSS and 700 MB of PHP heap.
  - After: 23 MB RSS and 3 MB of PHP heap.
  - The build fails loudly if the patch doesn't apply.
- **Background checkpointer process.** With WAL + `synchronous=NORMAL`, a commit is just a `write()`.
  The fsyncs happen during checkpoints, which SQLite normally runs inline in whichever request crosses
  1,000 WAL pages. The worker sets `wal_autocheckpoint=0`, and a separate process (`Server::addProcess`)
  with its own connection runs `PRAGMA wal_checkpoint(PASSIVE)` every second. It runs `TRUNCATE` once the
  WAL is large and fully copied back.
  - Measured p99: 4–5 ms with the checkpointer, 43 ms without.
  - At startup the same process reads through the hot indexes, to fault the DB file into the OS page cache.
- **Group commit.** Writes that arrive in the same event-loop iteration are queued and run at the end of
  it (`Swoole\Event::defer`) inside one `BEGIN IMMEDIATE … COMMIT`. Every response is sent only after
  that COMMIT (rule 6).
  - Likes-only load: about 10% less CPU per write.
  - Realistic mix: neutral, because writes are only about 8% of requests.
  - Batches grow when the server is busy, which is exactly when it matters.
- **PDO, not `ext-sqlite3`.** `SQLite3Stmt::execute()` steps the statement once, resets it, and then
  `fetchArray()` runs it again. So every SELECT does its first step twice, and `INSERT … RETURNING`
  **inserts the post twice**. The test suite can't see that, because the client gets the second id.
  PDO keeps the first stepped row. In interleaved runs it was 4–8% cheaper on reads and correct on writes.
- **JSON built in PHP, not in SQLite.** Letting SQLite render the response with
  `json_object`/`group_concat(... ORDER BY ...)` returns one finished string, but measured 11–13% more CPU
  per request than fetching rows and concatenating strings in PHP (`json_encode` only for the strings).
  Error bodies are constants.
- **JWT verified by hand on every request.** `hash_hmac('sha256')` with a constant-time `hash_equals`,
  HS256 only, and `exp`/`nbf` checked. Nothing is cached (rule 5).
- **SQL**: the reference queries.
  - A like is one statement, `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`.
  - A post-existence check runs only when it inserted nothing, to tell "already liked" (200) from "no such post" (404).
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 512 MiB `mmap_size`, 32 MiB page
  cache, and `temp_store=MEMORY`. Every statement's cursor is closed right after use, so no read
  transaction stays open and blocks the checkpointer.
- **PHP runtime**:
  - OPcache with the tracing JIT. It is roughly neutral here, since most of the time is spent in C, but it costs nothing.
  - `gc_disable()` in the worker, because request handling creates no reference cycles.
  - Swoole's POST, cookie and multipart parsing are switched off; the raw body is read and `json_decode`d once.

### Tried and rejected

- **2 worker processes**: 27% more CPU per request on one core (context switches, plus SQLite write-lock
  contention), with no better latency. One worker is the default; the `WORKERS` env var remains for experiments.
- **Swoole thread mode / ext-parallel (true threads)**: these need a ZTS PHP build. On one vCPU there is
  no parallel CPU to use, and the one thing worth doing in parallel, checkpoint fsyncs, is already a separate process.
- **Nginx**: see above.

## Numbers (local, Apple Silicon host, server container limited to `--cpus=1 --memory=2g`)

- `bench/load.js` with 10,000 VUs, plus 30,000 extra idle keep-alive connections (40,000 open in total):
  - p95 2.4 ms, p99 3.9 ms, 0 failed requests out of 183,055.
  - Server CPU 13–16%; container memory about 60 MB, not counting the DB page cache.
  - All 30,000 idle connections were still usable after 230 s.
- Fixed-rate request mix: about 50–60 µs of CPU per request, with 0 errors up to 13,000 req/s.
  A DigitalOcean vCPU is slower than these cores, so expect several times fewer req/s there.
  Still, 40,000 VUs only generate about 4,250 req/s.
- Crash test: 200 clients writing posts as fast as possible, then `kill -9` on the server.
  Every id that got a 201 (199,977 of them) was in the database afterwards, with no duplicates.

## License

MIT, under the repo's [license](../../LICENSE).
