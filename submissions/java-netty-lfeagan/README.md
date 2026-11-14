# Java + Netty

| | |
|---|---|
| Language | Java 25 (Eclipse Temurin 25.0.4.1+1) |
| Framework | Netty 4.2.19.Final, native epoll transport, with a hand-written HTTP/1.1 handler |
| SQLite driver | none: SQLite 3.54.0 compiled from the official amalgamation by `build.sh`, called through Java's FFM API (`java.lang.foreign`) |
| JSON, JWT | hand-written (strict JSON parser, HS256 via `javax.crypto.Mac`) |
| Build | Maven 3.9.16, all dependency and plugin versions pinned in `pom.xml` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: gcc, libc6-dev; Temurin JDK 25 and Maven from checksum-verified tarballs
bash build.sh          # compiles libsqlite3.so (sha256-verified source), then mvn package
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **Direct, not Nginx.** On one vCPU, Nginx's 14–20% comes straight out of the server's budget, and
  its `worker_connections 16384` caps the test at about 16k users. Netty on epoll holds tens of
  thousands of idle keep-alive connections for a few KB each. Idle connections are never timed out.
- **One thread does everything.** A single Netty event loop accepts, parses, verifies the JWT, runs
  SQLite and writes the response. Every query is an index lookup of a few microseconds, so handing
  work to a thread pool on one core would only add context switches and queueing.
- **No HTTP framework or codec.** Netty's `HttpServerCodec` allocates several objects per request.
  The handler parses only what this API needs (method, path, `Content-Length`, `Authorization`,
  `Connection`), straight from the receive buffer, and supports keep-alive and pipelining. Response
  headers are prebuilt bytes. All responses produced by one read go into one buffer that is
  flushed once.
- **SQLite through FFM instead of JDBC.** Rows are copied from SQLite's memory into the response
  buffer, JSON-escaped on the way, without creating a `String` or decoding and re-encoding UTF-8.
  Short C calls (`bind`, `column_*`, `reset`) are linked as critical downcalls, which skip the
  thread-state transition.
- **SQLite built with the recommended options**: `THREADSAFE=2` (each connection is used by one
  thread), `DEFAULT_MEMSTATUS=0`, `OMIT_SHARED_CACHE`, `OMIT_DEPRECATED`, `DQS=0`, and so on.
- **Group commit (rule 6).** The first write in an event loop iteration opens `BEGIN IMMEDIATE` and
  schedules a `COMMIT` to run after the iteration's I/O. Every like or post handled before that
  joins the same transaction. A connection with a write in flight holds back all of its responses
  until the commit returns, so no 201 is sent before its row is committed. If a commit fails, those
  connections are closed without a response.
- **Checkpoints on their own thread.** The serving connection has `wal_autocheckpoint=0`. A second
  thread with its own connection runs `PRAGMA wal_checkpoint(PASSIVE)` once a second, so a checkpoint's
  fsync never stalls the event loop.
- **SQL**: the reference queries with prepared statements. A like is one statement,
  `INSERT … SELECT ?,? WHERE EXISTS (post) ON CONFLICT DO NOTHING`. Only when it inserted nothing
  does a primary-key probe decide between 200 (already liked) and 404.
- **Pragmas**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, 32 MiB page cache.
- **JWT verified on every request** (rule 5): the HMAC key is set up once and the `Mac` reused, and
  the signature is computed over the token bytes in place and compared in constant time. HS256
  only; `exp` (and `nbf`, if present) checked.
- **JVM**: Serial GC (concurrent collectors only take CPU from the one core), a fixed 512 MB
  pre-touched heap, compact object headers, `-XX:-UsePerfData`, Netty leak detection off.

## License

MIT, under the repo's [license](../../LICENSE).
