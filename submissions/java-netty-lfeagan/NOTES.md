# Working notes

Notes for picking this up on another machine. `README.md` is the submission write-up; this file
holds the reasoning, test results and next steps.

## Status (2026-10-13)

- Passes `test/test.sh`: 42/42, run with `test/run.sh` in an Ubuntu 24.04 **arm64** container.
- Not yet run on **x86_64**. The amd64 branch of `install.sh` (JDK URL and sha256 from Adoptium's
  API) has never run. CI on the PR will be its first run.
- Branch `java-netty-lfeagan` is pushed to `origin` (the fork `lfeagan/twelve-dollar-server-challenge`).
  There is no PR upstream yet.

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

The Mac has JDK 17 only and no epoll, so everything runs in Docker on Ubuntu 24.04.

```bash
bash seed/make-seed.sh                                  # seed/feed.db + tokens.json (needs node, sqlite3)
docker run -d --name tds -v "$PWD":/repo ubuntu:24.04 sleep infinity
# Corporate network only (Zscaler TLS inspection): trust the Koch root CA in the container
security find-certificate -a -c "Koch Industries Inc RootCA-2014" -p > /tmp/koch-root.pem
docker cp /tmp/koch-root.pem tds:/usr/local/share/ca-certificates/koch-root.crt
docker exec tds update-ca-certificates
docker exec tds bash /repo/submissions/java-netty-lfeagan/install.sh
docker exec tds /opt/jdk-25/bin/keytool -importcert -noprompt -cacerts -storepass changeit \
  -alias koch-root -file /usr/local/share/ca-certificates/koch-root.crt   # so Maven trusts it too
docker exec tds bash -c 'apt-get install -y jq openssl sqlite3 && useradd -m dev'
docker exec -u dev -w /repo tds bash test/run.sh submissions/java-netty-lfeagan
```

The CA steps are only for the corporate network. Don't put them in `install.sh`.

Load testing locally (the server container capped like the droplet, k6 in a second container):

```bash
docker network create tdsnet; docker network connect tdsnet tds
docker update --cpus 1 --memory 2g --memory-swap 2g tds
# start the server inside tds with HOST=0.0.0.0 PORT=3000 on a fresh copy of feed.db, then:
docker run --rm --network tdsnet -v "$PWD":/repo -w /repo/bench --ulimit nofile=200000:200000 \
  grafana/k6:latest run -e BASE_URL=http://tds:3000 -e VUS=10000 -e RAMP_UP=30s -e DURATION=90s \
  -e TOKENS=/repo/seed/tokens.json load.js
```

## Results so far (Docker on an M-series Mac, so only a rough signal)

| Users | Result |
|---|---|
| 10,000 | 848 req/s, **0 errors** out of 135,674, p95 0.54 ms, p99 2.1 ms. The server used about 7% of its core and about 630 MB RSS. |
| 30,000 | Failed (3.6% errors, p99 of minutes), **caused by the load generator**. |

At 30k, k6 used 5.8 GB of the Docker VM's 7.7 GB and thrashed, while the server sat at 0.4–2% CPU
and stayed healthy. This laptop can't generate more than about 10–15k users, so real numbers need
real hardware.

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
4. **Confirm x86_64** with a clean `install.sh`, `build.sh` and `test/run.sh` on the droplet (CI
   will do this too).
5. **Open the PR** to the upstream challenge repo (`origin` is the fork) once the README has real
   numbers. CI must be green, and the PR must not touch files outside this folder.

## Possible edge cases (not covered by the tests)

- `GET /posts/` (empty id) returns 400 here; other implementations may return 404.
- `Authorization: Bearer` with nothing after it returns "missing bearer token" (the trailing space
  is trimmed).
- Ids with more than 18 digits return 400 (ids beyond 2^53 are out of scope).
- Leading-zero ids such as `/posts/007` are accepted as 7, like Python's `int()`.
