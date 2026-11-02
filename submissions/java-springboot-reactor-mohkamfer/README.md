# Java + Spring Boot + Project Reactor (Reactor Netty)

| | |
|---|---|
| Language | Java 25 (Eclipse Temurin 25.0.4.1+1, official tarball, checksum-pinned) |
| Framework | Spring Boot 4.1.1 running Reactor Netty (native epoll), handlers written against Project Reactor |
| SQLite | 3.53.4, compiled from the official amalgamation by `build.sh` (checksum-pinned), called through a small binding on Java's FFM API ([`Sqlite.java`](src/main/java/challenge/Sqlite.java)) |
| Build | Gradle 9.7.1 (official distribution, checksum-pinned); every dependency version is pinned in `build.gradle.kts` |
| JSON / JWT | hand-written (no Jackson, no JWT library) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # gcc, Temurin JDK 25 -> /opt/jdk, Gradle 9.7.1 -> /opt/gradle
bash build.sh          # build/native/libsqlite3.so + build/libs/app.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- **Spring Boot owns the server; the request path is pure Reactor Netty.** Boot builds, configures and
  runs the Reactor Netty server, and a `NettyRouteProvider` (Boot's own hook for raw Netty routes)
  sends every request to one handler, [`Api`](src/main/java/challenge/Api.java). There is no WebFlux
  and no Spring request/response adapter per request: those cost ~25% of the CPU in my measurements.
  `Api` dispatches by method and path itself, so unknown paths get the spec's `{"error":"not found"}`.
- **Reads run inline on the event loop.** Every read is an index or primary-key lookup that takes tens
  of microseconds from SQLite's `mmap`. On one vCPU, hopping to a worker pool only adds context
  switches, so reads run on the single Netty I/O thread with a thread-local connection and prepared
  statements, and are answered synchronously without building a Reactor chain.
  (`-Dapp.readers=N` moves them to a pool of N threads instead.)
- **One writer thread with group commit.** Writes go through one queue to one connection. The writer
  drains everything waiting (up to 256) into a single `BEGIN IMMEDIATE … COMMIT`, then completes each
  request's `Mono`. A 201 always means the row is committed (rule 6). A lone write just autocommits, and
  the statement is stepped to completion before the response is built.
- **SQL**: the reference queries. Each like is one statement,
  `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`; a post-existence check runs only when
  it inserted nothing, to tell "already liked" from "no such post". Posts use `INSERT … RETURNING`.
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL`, 1 GiB `mmap_size`, 2 MiB page cache per
  connection (reads come through mmap), `query_only` on reader connections, `SQLITE_OPEN_NOMUTEX`.

## Optimizations, and what each was worth

Measured as CPU time per request (from `/proc/<pid>/stat`) at a fixed 2,000 req/s of the load test's
request mix, plus resident memory, with the server in a 1-CPU / 2 GB container:

| Step | CPU / request | RSS (anon) |
|---|---|---|
| Spring WebFlux functional endpoint, sqlite-jdbc, 256 MiB heap | 139 µs | 245 MB |
| Plain Spring `HttpHandler` (no WebFlux exchange, filters, codecs) | 119 µs | 243 MB |
| Small fixed young gen + growable heap, 2 MiB SQLite cache, 1 malloc arena | ~117 µs | 161 MB |
| Drop the WebFlux starter entirely (`spring-boot-starter-reactor-netty`) | ~117 µs | 154 MB |
| Reactor Netty route via `NettyRouteProvider`, synchronous reads | 106 µs | 150 MB |
| JSON written as bytes; SQLite text copied as UTF-8, never decoded to `String` | 96 µs | 141 MB |
| Netty header validation off | ~91 µs | ~140 MB |
| **FFM SQLite binding + SQLite compiled with sqlite.org's recommended options** | **82 µs** | ~150 MB |

Details:

- **FFM instead of JNI.** sqlite-jdbc's JDBC/JNI glue was 13% of CPU. The FFM binding uses
  "critical" downcalls for in-memory calls (bind, column), and copies column text straight from
  SQLite's memory into the response buffer, escaping at the byte level.
- **SQLite build options** from sqlite.org's
  [recommended list](https://sqlite.org/compile.html#recommended_compile_time_options), notably
  `SQLITE_DEFAULT_MEMSTATUS=0` (no global mutex on every malloc) and `SQLITE_THREADSAFE=2` (no
  per-connection mutex; each connection is only ever used by one thread at a time).
- **JWT verified by hand** on every request with a thread-local `Mac`: HS256 only, constant-time
  signature compare, `exp`/`nbf` checked. Nothing is cached (rule 5).
- **JVM sized for 2 GB.** Serial GC (one core, no GC threads to schedule). A fixed 24 MiB young gen
  keeps the touched heap small. The old gen grows only with the number of open connections
  (~2.7 KB each). Compact object headers. One Netty I/O thread and one allocator arena.
  `MALLOC_ARENA_MAX=1`. The code cache has room enough that no code-cache full GC happens after
  warm-up.
- **Direct instead of Nginx.** Netty holds tens of thousands of idle keep-alive connections cheaply,
  so Nginx's CPU is better spent on the app. Reactor Netty has no server idle timeout by default, so
  keep-alive connections stay open.

## Local results

These numbers come from Docker on a desktop (i7-12700). k6 ran in separate containers; one k6
container tops out near 28k connections (ephemeral ports), so the biggest run used two. A $12
droplet's shared vCPU is slower than a desktop core, so I also ran with the server limited to 0.4 CPU
as a rough stand-in.

| `bench/load.js` | Server CPU | Errors | p95 | p99 | RSS anon / peak |
|---|---|---|---|---|---|
| 20,000 VUs | 1 | 0% | 0.38 ms | 1.1 ms | 187 MB / 234 MB |
| 20,000 VUs | 0.4 | 0% | 0.50 ms | 60 ms | 191 MB / 238 MB |
| 50,000 VUs | 0.4 | 0% | 19 ms | 138 ms | 258 MB / 305 MB |

## License

MIT, under the repo's [license](../../LICENSE).
