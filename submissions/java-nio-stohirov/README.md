# Java + its own epoll event loop

| | |
|---|---|
| Language | Java 25: Eclipse Temurin `jdk-25.0.4.1+1`, official tarball, sha256-pinned in `install.sh` |
| Framework | None. A hand-written HTTP/1.1 server on Linux `epoll`, called through FFM (no `java.nio` channels) |
| SQLite driver | No JDBC. Direct FFM (`java.lang.foreign`) bindings to SQLite **3.54.0**, compiled from the amalgamation by `build.sh` (sha256-pinned) |
| JSON | Responses are built by SQLite (`json_object` / `json_group_array`); a small strict parser reads request bodies and JWTs |
| Dependencies | Zero Java dependencies. `build.sh` needs only `cc` and the JDK |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl; Temurin 25 to /opt/jdk
bash build.sh          # SQLite -> build/libsqlite3.so, sources -> build/server.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The server runs on Linux only (it calls `epoll` directly).

## Optimizations, and why

**Process and I/O**
- **One thread, one SQLite connection, everything inline.** On one vCPU, more threads only add context
  switches. Every query is an index lookup that takes microseconds, so the event loop calls SQLite
  directly.
- **Sockets and epoll through FFM, not `java.nio`.** `accept4`, `epoll_wait`/`epoll_ctl`, `read`, `write` and
  `close` are FFM downcalls, the same way SQLite is called. `SocketChannel` and `Selector` added work on every
  connection: `fcntl` calls to make it non-blocking, two `getsockname` calls on accept, deferred deregistration
  and close in the selector, and several objects and locks per connection. A connection is now an `int` fd in a
  small object, and `close()` also removes it from the epoll set. At 256k simulated users this cut server CPU
  from 64% to 56% and moved the local ceiling from about 330k to about 390k users.
- **No buffers while idle.** One shared native read buffer and one shared response buffer serve every
  connection. A connection gets its own buffer only while it holds a partial request or a response the socket
  didn't take yet. 256,000 open connections leave about 10 MB of live heap.
- **Direct, no Nginx.** Nginx would take CPU from the one core, and `worker_connections 16384` caps it at
  about 16k users.
- **Past the descriptor limit: idle eviction.** Connections sit in an LRU list. Within 128 descriptors of the
  `LimitNOFILE` limit (65,407 connections at 65,535), the server closes the 64 longest-idle connections that
  have nothing in flight, so `accept` never fails with `EMFILE`. A client that was evicted while idle just
  reconnects on its next request. Eviction starts this late on purpose: on a 2 GB box the kernel's connection
  tracking table (65,536 entries) is full at about the same point, and evicting below it would only churn
  connections that other servers simply keep.
- **Evictions reset instead of closing gracefully.** An evicted connection gets `SO_LINGER=0` before `close()`,
  so the close sends RST: no FIN exchange and no TIME_WAIT. Past ~64k users nearly every request reconnects.
- **Adaptive wake-up batching.** When a wake-up brought fewer than 32 events, the loop parks for 2 ms so
  the next `epoll_wait` returns many at once. With ~0.1 req/s per user, most wake-ups would otherwise
  carry a single request. This saves about 10% of CPU for about 2 ms of latency. Busy wake-ups don't
  park, so the pause never caps throughput.
- **Listen backlog** of 4096.

**SQLite**
- **JSON built in SQL.** Each post object comes out of SQLite as one TEXT column (`json_object`), copied
  native-to-native into the response buffer: no Java strings, no per-column calls, no escaping in Java.
- **Feed rows are joined in Java, not by `json_group_array`.** The feed returns one row per post in index
  order, and Java writes `{"posts":[`, the commas and `]}` around them. Aggregating in SQL needed
  `json_group_array(… ORDER BY …)`, which builds a temporary sort b-tree on every call and copies the JSON
  twice more. Splitting it cut the feed query from ~13.9 µs to ~9.7 µs and server CPU per request by ~14%.
- **FFM instead of JDBC.** Statements are prepared once (`SQLITE_PREPARE_PERSISTENT`) and reused.
  Short calls (bind, column, changes) are `Linker.Option.critical` downcalls.
- **`SQLITE_ENABLE_QPSG`.** Without it, the `GET /posts/:id` statement (`p.id = ?1` plus a correlated count)
  was re-prepared on every bind, because SQLite thought the bound value might change the plan.
- **Group commit.** Likes and new posts are queued during a wake-up and run together in one
  `BEGIN IMMEDIATE … COMMIT`. Their responses are written only after `COMMIT` returns (rule 6). Requests
  pipelined behind a write are answered after it, in order.
- **Checkpoints on our schedule.** `wal_autocheckpoint=0`. A PASSIVE checkpoint
  (`sqlite3_wal_checkpoint_v2`) runs between batches about once a second, never inside a request's
  commit.
- **Pragmas:** `locking_mode=EXCLUSIVE` (no shared-memory WAL index and no file locking per transaction),
  `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `cache_size=-65536`, `temp_store=MEMORY`,
  `journal_size_limit=64MB`.
- **Compile options:** `-O2 -march=native` with `THREADSAFE=0`, `DEFAULT_MEMSTATUS=0`,
  `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `OMIT_DECLTYPE`,
  `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `USE_ALLOCA`,
  `ENABLE_QPSG`.

**Requests**
- **HTTP parsing on raw bytes.** It reads only the request line, `Content-Length`, `Authorization` and
  `Connection`. Status lines and headers are precomputed, and each response goes out in one `write`.
- **JWT verified in full on every request** (rule 5). One keyed `javax.crypto.Mac` is reused, the
  signature is base64url-encoded straight from the digest and compared in constant time, and the token
  must be HS256 with valid `exp`/`nbf`.

**JVM**
- `-XX:+UseSerialGC` (no GC threads competing for the core), a fixed 256 MB heap, `-XX:+AlwaysPreTouch`,
  `-XX:+UseCompactObjectHeaders`, and capped metaspace, code cache and direct memory. Steady state
  allocates about 1 MB/s, so a young GC happens every couple of minutes.

## Local measurements

These were measured on Docker on an Apple Silicon laptop, not the droplet. The server was pinned to one core
with `--cpus 1 --memory 2g`, no swap and `nofile=65535`. Load came from a load generator replaying
`bench/load.js`'s user loop with one keep-alive connection per user, with k6's rules for latency, timeouts and
retries. Each level was a 60 s ramp and a 2-minute hold.

| Users | req/s | p95 | p99 | Errors | Server CPU |
|---|---|---|---|---|---|
| 128,000 | 13,543 | 11 ms | 28 ms | 0.011% | 31% |
| 256,000 | 27,086 | 38 ms | 55 ms | 0.0013% | 56% |
| 320,000 | 33,808 | 57 ms | 68 ms | 0.0009% | 69% |
| 352,000 | 37,106 | 90 ms | 127 ms | 0.0006% | 76% |
| 384,000 | 40,578 | 119 ms | 176 ms | 0.0075% | 78% |
| 416,000 | fails: the core saturates at ~40k req/s | | 6.7 s | 0.34% | 82% |

## License

MIT, under the repo's [license](../../LICENSE).
