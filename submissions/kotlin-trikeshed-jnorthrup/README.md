# kotlin-trikeshed-jnorthrup

Kotlin with no framework: RelaxFactory's `one.xio` NIO reactor, monomorphized, on one thread, calling SQLite through
`java.lang.foreign`.

| | |
|---|---|
| Language | Kotlin 2.4.21 (Kotlin Multiplatform Gradle project, JVM target 25) |
| Runtime | Oracle GraalVM 25.4.4.1.1 (JDK 25.0.4.1.1, Graal JIT), the latest release on graalvm.org (25.4), from gds.oracle.com, installed by `install.sh` |
| Build | Gradle 9.6.1, official distribution, installed by `install.sh` |
| HTTP | own: RelaxFactory's `one.xio` selector loop with a single visitor class, on `java.nio` (epoll) |
| SQLite | 3.54.0 amalgamation, compiled by `build.sh`; the download is checked against the SHA3-256 sqlite.org publishes |
| Driver | `java.lang.foreign` (FFM) downcalls into that `libsqlite3`; no JDBC, no JNI |
| Libraries | TrikeShed (HMAC-SHA256 for JWT, SHA-3), compiled from the git subrepo `trikeshed/TrikeShed`: the files listed in `trikeshed/sources.txt`, at the commit git records for the subrepo |

## Nginx or direct

Direct: run with `HOST=0.0.0.0 PORT=80`.

## What happens per request, and why

- **One thread.** The selector thread accepts, reads, parses, queries SQLite and writes. The box has 1 vCPU, so there
  are no locks and no hand-offs between threads.
- **Group commit (rule 6).** The first write opens `BEGIN IMMEDIATE`, and later writes join it until the transaction is
  64 ms old; the selector waits at most until then. Then there is one `COMMIT`, and only then are the responses held
  since the transaction opened written (reads inside it included), so a 201 always follows its commit. Measured with
  this repo's load model at 64,000 users against a 1-CPU / 2 GB container (2 min holds, alternating runs), server CPU
  per request: one `COMMIT` per selector pass 67.1-69.6 us (p95 10-17 ms); a 32 ms window 59.7-62.1 us (p95 52-59
  ms); 64 ms 56.2-59.0 us (p95 89-104 ms, p99 122-150 ms); 128 ms 56.3-56.4 us (p95 180-183 ms, p99 309-399 ms). A
  5-minute hold at 64,000 users with the 64 ms window: 56.2 us per request, 6,736 requests/s, p95 95 ms, p99 124 ms,
  max 190 ms, no errors.
- **SQLite settings.** `journal_mode=WAL`, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE` (the wal-index lives in
  process memory, with no `-shm` file), `mmap_size` 1 GiB, `cache_size` 16 MiB, `temp_store=MEMORY`. Statements are
  prepared once with `SQLITE_PREPARE_PERSISTENT` and reused. They cache plans, not data (rule 5).
- **SQLite build.** `SQLITE_THREADSAFE=0` because one thread owns the connection. From the list at
  sqlite.org/compile.html#rcmd: `DQS=0`, `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`, `LIKE_DOESNT_MATCH_BLOBS`,
  `MAX_EXPR_DEPTH=0`, `OMIT_DECLTYPE`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`. Also
  `USE_ALLOCA`, `OMIT_LOAD_EXTENSION` and `ENABLE_QPSG`. Without the query planner stability guarantee, SQLite 3.54.0
  re-prepares the post-by-id statement every time a different id is bound (`sqlite3ExprIsInteger` marks the
  parameter); under load that re-prepare took 17% of the server's CPU.
- **FFM downcalls** are linked as `critical`: there is no thread-state transition per call, and Kotlin byte arrays go to
  `sqlite3_bind_text` in place.
- **HTTP.** Only `Content-Length`, `Authorization` and `Connection` are read (RelaxFactory's header interest), and other
  headers are skipped. Pipelined requests on one connection are answered with one write. Idle keep-alive connections
  stay open.
- **Responses.** JSON is written from byte templates into one reused buffer. Text columns are copied out of SQLite and
  escaped in place.
- **JWT.** The HS256 signature is checked on every request (rule 5) with a constant-time compare. `alg` must be `HS256`,
  and `exp` is checked.
- **Queries.** The post, feed and create queries are the ones in SPEC.md. A like is one statement,
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`, plus `sqlite3_changes()`. The post is looked up
  separately only when nothing was inserted.
- **JVM.** `-XX:+UseSerialGC -Xms128m -Xmx384m`.

## Layout

| Path | |
|---|---|
| `server/src/commonMain` | HTTP framing, JSON scanner and writer, JWT, routing (`Feed.kt`) |
| `server/src/jvmMain` | `Xio.kt` (reactor), `Sqlite.kt` (FFM binding and statements), `SqliteSource.kt` (fetch + SHA3-256 check) |
| `trikeshed/` | Gradle module `:trikeshed`: `build.gradle.kts` and `sources.txt`, no sources of its own |
| `trikeshed/TrikeShed` | git submodule registered in `.gitmodules`: jnorthrup/TrikeShed at the commit git records. Initialize it with `git submodule update --init`; if it is empty, `build.sh` fetches that same recorded commit (`git` from `install.sh`) |
| `install.sh`, `build.sh`, `start.sh` | the challenge contract |
| `tester.sh` | a tester's walk through git, the TrikeShed linkage, scaffolding, and each edition's build, tests and run, into `build/editions/<os>-<arch>/` |
