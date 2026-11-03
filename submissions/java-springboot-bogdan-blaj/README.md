# Java + Spring Boot + Netty + SQLite (FFM)

| | |
|---|---|
| Language | Java 25 (Eclipse Temurin 25.0.4.1+1) |
| Build | Apache Maven 3.9.16 |
| Framework | Spring Boot 4.1.1 (`spring-boot-starter` only: no web, WebFlux, Reactor or Jackson) |
| HTTP | Netty 4.2.17.Final (`netty-codec-http`, native epoll transport), used directly |
| SQLite | 3.53.4, built from the official amalgamation by `build.sh` and called through the Java FFM API (no JDBC) |
| **Nginx or direct** | **Direct**: listens on `HOST:PORT` = `0.0.0.0:80` itself, no Nginx |

SPEC: in direct mode every k6 user holds its own keep-alive connection (up to about 15,000 open connections), and
the service gets `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE`, so it binds port 80 without running as root.

Spring Boot runs the process (configuration, lifecycle, logging, shutdown hook) with `WebApplicationType.NONE`;
the HTTP server is a raw Netty pipeline started from a `SmartLifecycle` bean.

## Running it

```bash
sudo bash install.sh   # apt build-essential, curl; Temurin JDK -> /opt/jdk-25, Maven -> /opt/maven (checksummed)
bash build.sh          # SQLite amalgamation (sha256-checked) -> native/libfeedsqlite.so, then mvn package -> target/feed.jar
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` supports x86_64 and aarch64. `build.sh` skips steps that are already up to date. `start.sh` reads only
the SPEC environment variables and `exec`s the JVM in the foreground. Locally it is healthy about 3 s after start
(Spring context, database open and prewarm).

SQLite compile flags: `-O2 -march=native -fPIC -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
-DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION
-DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1`.

## Design, and why

- **One thread does everything.** A single Netty epoll event loop accepts, parses, verifies JWTs, runs SQLite,
  writes responses, commits and checkpoints. The box has one vCPU, so worker pools or a separate database thread
  would only add wakeups and handoffs. Backpressure is TCP.
- **SQLite inline on the event loop through FFM.** One connection, statements prepared once, response JSON built
  by SQLite (`json_object`) and copied from `sqlite3_column_blob` straight into a pooled direct `ByteBuf`. Short
  calls (`bind_*`, `column_*`, `changes`) use critical downcalls. SQLite is built with `SQLITE_THREADSAFE=0`;
  every database entry point checks that it runs on the loop thread.
- **Feed query.** `GET /feed` steps 20 rows of `json_object(...)` from
  `posts ORDER BY created_at DESC, id DESC LIMIT 20` joined to `users` (with the `like_count` subquery). The plan
  walks `posts_created_at_id_idx` and stops after 20 rows, with no sort step. Java writes `{"posts":[`, the rows
  separated by commas, and `]}` into the response buffer and fills in the header (Content-Length) in front of the
  body at the end, so the body is copied once. This was cheaper than building the whole body with `group_concat`
  in SQLite (about 16 us instead of 22 us per feed in the data-layer benchmark).
- **Group commit, one transaction per event-loop pass.** The first statement of a pass runs `BEGIN`; every
  response produced in that pass is held in arrival order; a task that Netty runs after the pass
  (`executeAfterEventLoopIteration`) runs `COMMIT` and only then writes and flushes the held responses. No write
  success leaves before its commit returned `SQLITE_DONE`; if the commit fails, every held response from that
  transaction becomes 500. Reads join the pass's transaction too, which saves SQLite's per-statement read
  transaction setup. Constraint failures fail only their own request.
- **WAL, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, manual checkpoints.** With exclusive locking the WAL index
  lives in heap memory (no `-shm` file) and nothing else can open the database. Automatic checkpoints are off; a
  `wal_hook` upcall records the WAL size, and after a pass has flushed its responses a `RESTART` checkpoint runs
  once the WAL has 1,000 frames (about 4 MiB). Larger thresholds (2,048 and 4,096) gave worse p99 locally.
  Other pragmas: `cache_size=500` pages, `mmap_size=1 GiB`, `temp_store=MEMORY`, `foreign_keys=ON`.
- **Prewarm.** Before binding, the database file is read once into the OS page cache, every table and index is
  scanned once through mmap, and each read statement runs once. Nothing is cached in the process (rule 5).
- **No per-request allocation in our code.** JWT verification (HS256 computed with one reused `MessageDigest` and
  precomputed key pads, because `javax.crypto.Mac.doFinal` allocates; constant-time compare; `exp`/`nbf`, `sub` and
  `username` checks) and the request-body parser (strict RFC 8259, UTF-8 validation, JavaScript `trim()` whitespace
  set, code-point length) work on bytes with reused scratch arrays. Fixed error
  responses are prebuilt; other headers are templates plus digits. What remains per request is Netty's request
  and header objects.
- **HTTP details.** Keep-alive by default, `Connection: close` and HTTP/1.0 honoured, pipelined requests answered
  in order, chunked bodies, `Expect: 100-continue` answered after the headers (in order with earlier pipelined
  responses), bodies over 16 KiB get 413, malformed request heads get 400 and are closed.
- **Connection guard sized from the descriptor limit.** Open connections are kept in an intrusive LRU list; a sweep
  every 5 s closes connections idle (or stuck mid-request) for 120 s. Above `min(65000, nofile - reserve)` open
  connections (65,000 under `nofile=65535`), responses carry `Connection: close`, so `accept()` always finds a free
  descriptor; up to that many users keep their connection and never see a close. Users above the cap reconnect for
  their next request: locally (k6 `bench/load.js`, 5,000 users, two thirds of them above a lowered cap) that cost
  0 errors, no p95/p99 change and about 12-37 us of CPU per reconnect. Accept errors are logged at most once per 10 s.
- **JVM flags** (`start.sh`): `-XX:ActiveProcessorCount=1 -XX:+UseSerialGC` (one core, no GC threads),
  `-Xms640m -Xmx640m -Xmn160m -XX:+AlwaysPreTouch` (fixed, pre-touched heap, so memory does not grow under
  load), `-XX:+UseCompactObjectHeaders`, `-XX:MaxMetaspaceSize=160m`, `-XX:MaxDirectMemorySize=128m`,
  `-XX:+DisableExplicitGC`, `-Xss512k`, Netty pooled allocator with one direct and one heap arena, leak detection off.

Memory budget on the 2 GB box: the JVM process is about 890 MB RSS after startup (640 MiB of it is the
pre-touched heap), about 930 MB under load, and about 905 MB with 20,000 idle keep-alive connections (live heap
after a full GC: 42 MB). Kernel socket buffers come on top. The database mmap is file-backed page cache that the
kernel can reclaim.

Known edge case: a body starting with NUL (`"\u0000x"`) passes validation but fails the schema's `length(body)`
CHECK (SQLite counts characters up to the first NUL), so it returns 500. The schema is fixed, so this is left as is.

## Local results (not the droplet)

These numbers are from Docker Desktop on an Apple Silicon Mac (linux/arm64), so they are only useful as relative
comparisons. Server container: 1 CPU (`--cpuset-cpus=0 --cpus=1`), 2 GB, `nofile=65535`, fresh seed per run.
Load: `wrk` with 2 threads and 64 keep-alive connections on other CPUs, 10 s warm-up, 20 s measured. "Mixed" is
100 `GET /feed` : 100 `GET /posts/:id` : 15 likes : 2 creates, with real writes. Median of 3 alternating runs per
row; CPU is the server process's user + system time divided by the number of requests.

| Scenario | Build | req/s | p50 | p99 | CPU per request |
|---|---|---:|---:|---:|---:|
| `GET /feed` | earlier build: single-row `group_concat` feed, reads in autocommit | 28,201 | 2.14 ms | 5.94 ms | 35.4 us |
| `GET /feed` | tuned build | 48,608 | 1.13 ms | 4.11 ms | 20.6 us |
| mixed | earlier build: single-row `group_concat` feed, reads in autocommit | 47,605 | 1.20 ms | 20.79 ms | 18.1 us |
| mixed | tuned build | 57,088 | 0.95 ms | 20.66 ms | 14.6 us |

The mixed p99 is dominated by checkpoint `fdatasync` time, which is slow inside Docker on the Mac. A later code
cleanup (no behaviour change) measured within noise of the tuned build.

## Validation (local)

- `test/run.sh`: 42/42 on linux/arm64, and 42/42 on linux/amd64 (Ubuntu 24.04 container under Rosetta, clean
  `install.sh` and build).
- Official `bench/load.js` with k6 2.3.0, unchanged, 5,000 users (30 s ramp, 3 min hold): 115,485 requests,
  0 failed, p95 1.4 ms, p99 3.8 ms.
- Crash durability: 200 acknowledged creates and likes, then SIGKILL and restart: all 200 present.
- 64,000 idle keep-alive connections held and reused, about 1.6 KiB of heap per connection.
- Descriptor pressure: under `nofile=512`, connections above the guard are served and closed, and keep-alive
  resumes when the pressure ends.

## License

MIT, under the repository's [license](../../LICENSE).
