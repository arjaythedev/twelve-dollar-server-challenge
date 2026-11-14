# Working notes

Notes for picking this up on another machine. `README.md` is the submission write-up; this file
holds the reasoning, test results and next steps.

## Status (2026-10-13)

- Passes `test/test.sh`: 42/42, run with `test/run.sh` on Ubuntu 24.04 on **arm64** (native) and on
  **x86_64** (amd64 container under OrbStack's Rosetta emulation).
- x86_64: the amd64 branch of `install.sh` ran clean (JDK 25 and Maven checksums verified), `build.sh`
  fetched the `linux-x86_64` epoll jar, and the server started on epoll, not the NIO fallback. The
  emulated run says nothing about speed. CI on the PR will be the first native x86_64 run.
- Load tested on arm64 up to 60k users with 0 failures (see Results). Not yet run on a real droplet,
  and no 5-minute hold yet.
- Branch `java-netty-lfeagan` is pushed to `origin` (the fork `lfeagan/twelve-dollar-server-challenge`).
  There is no PR upstream yet. Before opening one: add real numbers to `README.md`, and decide
  whether to leave this file out of the PR (it's working notes, not a required file).

## The numbers that drive the design

**Load per user.** `bench/load.js` loops feed, think 3–7 s, one post, think 3–8 s, like (15%),
create (2%), idle 5–15 s. That is about 20.5 s and 2.17 requests per loop, so about
**0.106 req/s per user**:

| Users | Requests/s |
|---|---|
| 10k | ~1,060 |
| 20k | ~2,100 |
| 50k | ~5,300 |

A lean server on one core can do well over 10k req/s on these queries, so **CPU is probably not
the first wall**.

**The likely ceiling is connections.**
- Direct: each k6 user holds its own keep-alive connection, and `LimitNOFILE=65535` caps us at
  roughly **65k users**.
- Behind Nginx: `worker_connections 16384` caps it near **16k users**, which is why we serve direct.

So what matters is cheap per-connection memory, a flat p99 while about 1,000 new connections per
second arrive during the 60 s ramp, and not running out of RAM at 50–60k sockets.

**Scoring ramp.** 1k-user warm-up for 2 min, then 2,500 users doubling (5k, 10k, 20k, 40k, 80k)
until a run fails, then narrowing to within 250. The score is the most users that pass a 5-minute
hold with p95 < 500 ms, p99 < 1 s and < 1% errors.

## Design decisions, and why

- **Direct on `:80`, no Nginx, no Spring.** Nginx costs 14–20% CPU (per the video) and caps
  connections, and Spring is too much overhead.
- **One Netty epoll event loop does everything**: accept, parse, JWT, SQLite, respond. On one vCPU,
  thread handoffs only add context switches. Every query takes microseconds.
- **A hand-written HTTP handler** (`HttpHandler`) instead of `HttpServerCodec` and
  `HttpObjectAggregator`, which allocate per request. It reads the method, path, `Content-Length`,
  `Authorization` and `Connection`, and supports keep-alive and pipelining. Responses from one read
  go into one buffer, and flushing happens in `channelReadComplete`. Chunked request bodies close the
  connection (out of scope).
- **SQLite via FFM, not xerial sqlite-jdbc** (`Sqlite.java`). The amalgamation is compiled in
  `build.sh`. Text columns are copied from `sqlite3_column_text` into the response buffer with JSON
  escaping and no `String`. Short calls are `Linker.Option.critical(false)`; step, prepare, exec
  and open are regular downcalls because they can do I/O. sqlite-jdbc is the fallback if FFM ever
  causes trouble.
- **Group commit** (`App.joinWrite` and `commitAll`): the first write in an event-loop iteration
  runs `BEGIN IMMEDIATE` and schedules `COMMIT` with `loop.execute`, which runs after that
  iteration's I/O. A connection with a pending write holds back all of its responses (`held`) until
  the commit, which keeps pipelined responses in order. If the commit fails, the connection is
  closed with no response. Known wrinkle: reads served while the transaction is open see its
  uncommitted rows. That only matters if a commit fails.
- **Checkpoints on their own thread and connection**: `wal_autocheckpoint=0` on the serving
  connection, and `PRAGMA wal_checkpoint(PASSIVE)` every 1 s in `Server.startCheckpointer`, so
  checkpoint fsyncs never block the event loop. Checked under load: the WAL restarts as it should.
  The file stays at its high-water mark; `journal_size_limit` would trim it, but that isn't needed.
- **SQL**: the reference post and feed queries (the plan uses `posts_created_at_id_idx`, the users
  primary key, and the covering index `likes_post_id_idx`). A like is
  `INSERT … SELECT ?1,?2 WHERE EXISTS(post) ON CONFLICT DO NOTHING`, then `changes()`; only when
  nothing was inserted does a primary-key probe choose 200 or 404.
- **JWT** (`Jwt.java`): the `Mac` is keyed once and reused (rule 5 bans caching results, not keys).
  The token is copied to a scratch array, the header is parsed and `alg` must be `HS256`, and the
  signature is compared in constant time. `exp` and `nbf` are checked if present. A missing `exp` is
  accepted, like PyJWT and jsonwebtoken do. `sub` must be a positive integer string and `username`
  a string, or the result is "invalid token payload".
- **Body validation**: `String.strip()` for trimming, and the 500 limit counts **code points**
  (matching SQLite's `length()` CHECK), not UTF-16 units. `strip()` doesn't remove NBSP or BOM the
  way JS `trim()` does; that isn't tested.
- **JVM**: Serial GC (concurrent collectors steal from the one core), `-Xms512m -Xmx512m`,
  AlwaysPreTouch, compact object headers, `-XX:-UsePerfData`, `-Xss512k`, MaxDirectMemory 256m,
  Netty leak detection off. `--enable-native-access=ALL-UNNAMED` and
  `--sun-misc-unsafe-memory-access=allow` silence the JDK 25 warnings.
- **Rejected**: GraalVM native-image (building it on a 2 GB box with no swap would likely run out
  of memory), virtual threads, more than one event loop, and self-warm-up at startup (rule 7 risk;
  the 2-minute warm-up is enough for C2).

## Local dev setup (macOS)

The Mac has JDK 17 only and no epoll, so everything runs in Docker on Ubuntu 24.04. Docker here is
OrbStack (`brew install --cask orbstack`); its VM memory cap is `orbctl config show | grep memory_mib`
(16 GB now). Node isn't installed on the Mac, so the seed is generated inside the container.

```bash
docker run -d --name tds -v "$PWD":/repo ubuntu:24.04 sleep infinity
docker exec tds bash -c 'apt-get update -qq && apt-get install -y -qq nodejs sqlite3 jq openssl curl ca-certificates && useradd -m dev'
docker exec -u dev -w /repo tds bash seed/make-seed.sh  # seed/feed.db + tokens.json (about a minute)
docker exec tds bash /repo/submissions/java-netty-lfeagan/install.sh
docker exec -u dev -w /repo tds bash test/run.sh submissions/java-netty-lfeagan
```

Load testing locally (the server container capped like the droplet, k6 in a second container):

```bash
docker network create tdsnet; docker network connect tdsnet tds
docker update --cpus 1 --memory 2g --memory-swap 2g tds
# start the server inside tds on a fresh copy of feed.db (run test/run.sh first so target/ is built):
docker exec tds bash -c 'cp /repo/seed/feed.db /tmp/bench.db && chown dev /tmp/bench.db'
docker exec -d -u dev -w /repo/submissions/java-netty-lfeagan tds bash -c 'ulimit -n 65535; \
  SQLITE_PATH=/tmp/bench.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 exec bash start.sh > /tmp/server.log 2>&1'
# then k6:
docker run --rm --network tdsnet --sysctl net.ipv4.ip_local_port_range="1024 65535" \
  -v "$PWD":/repo -w /repo/bench --ulimit nofile=200000:200000 \
  grafana/k6:latest run -e BASE_URL=http://tds:3000 -e VUS=30000 -e RAMP_UP=60s -e DURATION=90s \
  -e TOKENS=/repo/seed/tokens.json load.js
```

Two things bit us, and both are in the load generator, not the server:
- **k6 memory**: about 190 KB per user (5.8 GB at 30k, so about 12 GB at 60k). The Docker VM needs
  more than that, plus 2 GB for the server container.
- **Source ports**: the default range (32768-60999) holds about 28k connections to one address and
  port. Past that, k6 logs `cannot assign requested address` and `GET /feed` fails. The `--sysctl`
  above widens it to about 64.5k, which is the most one k6 container can reach. Beyond that, use a
  second client address or several k6 containers (each slice of `tokens.json` must differ).

Restart the server on a fresh database copy between runs, so writes from one run don't carry over.

## Results so far (Docker on an M-series Mac, so only a rough signal)

Server container capped at 1 CPU, 2 GB, no swap, arm64, epoll, 90 s hold. k6 shares the Mac's CPU.
All runs: **0 failed requests**, every threshold passed.

| Users | Ramp | Requests/s | p95 | p99 | Server peak CPU | Server RSS | Open fds |
|---|---|---|---|---|---|---|---|
| 10,000 | 30 s | 848 | 0.54 ms | 2.1 ms | ~7% | ~630 MB | |
| 30,000 | 30 s | 2,536 | 1.43 ms | 2.25 ms | 37% | 617 MB | |
| 40,000 | 60 s | 3,181 | 1.68 ms | 2.58 ms | 39% | 624 MB | 40,023 |
| 50,000 | 60 s | 3,979 | 1.90 ms | 2.92 ms | 43% | 617 MB | 50,023 |
| 60,000 | 60 s | 4,766 | 1.91 ms | 8.66 ms | 46% | 619 MB | 60,023 |

- **Memory is flat**: about 600-620 MB from 30k to 60k users, so heap and buffer tuning isn't needed
  yet. Extra connections cost almost nothing.
- **CPU is not the wall**: 46% of one core at 60k.
- **File descriptors are the first hard ceiling**: 60,023 of 65,535 at 60k (about 92%). 80k users
  can't fit, whatever else is true. Rule 11 means we can't raise `LimitNOFILE`.
- **p99 rose from 2.9 ms to 8.7 ms at 60k.** Still far below the 1 s limit; the cause isn't
  confirmed (60k sockets sharing the core with k6 and the OS is the guess).
- **Earlier failed runs were the load generator**: the first 30k run (7.7% errors, all
  `cannot assign requested address`) was source-port exhaustion in k6, and the run before that
  (3.6% errors) was k6 running out of VM memory. See the local setup section for the fixes.
- Not yet done: the real 5-minute hold, x86_64, a real droplet. Longer runs are planned later.

## Next steps

1. **Benchmark on real hardware.** Rent a $12 DigitalOcean droplet (1 vCPU, 2 GB, Ubuntu 24.04
   x86_64) for the server and a larger droplet in the same region for k6. Run `install.sh` and
   `build.sh`, start on port 80 with `LimitNOFILE=65535`, and work through the real ramp: warm-up at
   1k, then 20k, 40k and 60k users. Watch CPU, RSS, `ss -s`, and `/proc/net/sockstat` (kernel TCP
   memory).
2. **Find what runs out first**: CPU, file descriptors (about 65k), or memory (JVM plus kernel
   socket buffers plus page cache for the 250 MB DB, inside 2 GB with no swap).
3. **Tuning experiments, each A/B tested on the droplet:**
   - Netty io_uring transport (`netty-transport-native-io_uring`) against epoll at 40–60k connections.
   - Heap size: try 256–384 MB if memory is tight, and watch GC pauses with `-Xlog:gc`.
   - Netty allocator: the 4.2 default (adaptive) against pooled with `numDirectArenas=1`.
   - The receive buffer allocator size per connection.
   - Whether group commit actually helps compared with autocommit (measure p99).
   - `cache_size` and `mmap_size`.
4. **Confirm x86_64 natively** with a clean `install.sh`, `build.sh` and `test/run.sh` on the droplet
   (CI will do this too). An emulated amd64 run already passed 42/42.
5. **Open the PR** to the upstream challenge repo (`origin` is the fork) once the README has real
   numbers. CI must be green, and the PR must not touch files outside this folder.

## Possible edge cases (not covered by the tests)

- `GET /posts/` (empty id) returns 400 here; other implementations may return 404.
- `Authorization: Bearer` with nothing after it returns "missing bearer token" (the trailing space
  is trimmed).
- Ids with more than 18 digits return 400 (ids beyond 2^53 are out of scope).
- Leading-zero ids such as `/posts/007` are accepted as 7, like Python's `int()`.
