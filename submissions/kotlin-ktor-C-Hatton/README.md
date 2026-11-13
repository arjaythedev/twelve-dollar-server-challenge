# Kotlin + Ktor

| | |
|---|---|
| Language | Kotlin 2.4.21 on OpenJDK 25 (Ubuntu 24.04's `openjdk-25-jdk-headless` package) |
| Framework | Ktor 3.6.0, Netty engine (Netty 4.2.17, native epoll transport) |
| SQLite driver | Direct bindings to the SQLite C API via Java's FFM API (`java.lang.foreign`), against SQLite 3.53.4 built by `build.sh` from the official amalgamation (pinned by sha256) |
| JSON | Responses written by hand into a byte buffer; request bodies and JWTs parsed with kotlinx.serialization 1.11.0 |
| Build | Gradle 9.8.1 (official zip, sha256-checked in `install.sh`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: openjdk-25-jdk-headless, gcc; Gradle 9.8.1 into /opt
bash build.sh          # compiles SQLite into build/native, then the server into build/install
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

Measured on a 1-CPU/2 GB container (Ubuntu 24.04, server pinned to one core) with `bench/load.js`.
At 50,000 users: 0 errors, p95 0.46 ms, p99 8 ms, about 30 µs of CPU per request. The request
mix with no think time saturates at about 48k requests/s on one core.

**One thread does everything.** Ktor's Netty engine runs with a single event-loop thread that
parses each request, runs the handler, calls SQLite inline (every query is an index lookup that
takes microseconds) and writes the response; a second thread only accepts connections. With
`shareWorkGroup`, Ktor sizes the worker group as `workerGroupSize + callGroupSize`, so
`callGroupSize = 0` is what keeps it at one thread. That matters more than it looks: it means one
SQLite connection, and SQLite throws away a connection's whole page cache whenever *another*
connection has written. With two threads, every like reset the other thread's cache.

**SQLite through FFM, not JDBC.** The xerial JDBC driver spends a third of its time outside SQLite:
JNI transitions, per-query column-name lookups, byte-array copies, and a `SELECT last_insert_rowid()`
it prepares after every INSERT. The bindings in `Sqlite.kt` call `sqlite3_step` and friends
directly, with the short calls (`column_*`, `bind_*`, `reset`) as critical downcalls. Text columns
are copied as raw UTF-8 bytes straight into the response buffer. This took saturation throughput
from 34k to 46k requests/s.

**SQLite built for this.** `build.sh` compiles the amalgamation with SQLite's recommended options:
`SQLITE_DEFAULT_MEMSTATUS=0` (no global mutex on every malloc), `SQLITE_THREADSAFE=2` (no
per-connection mutex; each connection belongs to one thread), `HAVE_USLEEP`, and the `OMIT_*`
options for features we don't use.

**SQL.** The reference queries, as persistent prepared statements. A like is one statement,
`INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. A post-existence check runs only
when it inserted nothing, to tell "already liked" from "no such post".

**Pragmas.** `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), a 64 MiB page cache, and **no
mmap**: every checkpoint that grows the file makes SQLite remap it, and the page faults afterwards
cost about 20% of throughput. Each write commits in autocommit mode before its response is sent.
Checkpoints stay inline on the one writing connection. I tried a background checkpoint thread, but
under steady writes its PASSIVE checkpoints never catch up to a moment with no new frames, so the
WAL is never rewound and grows without bound (1.4 GB in a minute at saturation).

**Direct, and the open-file limit.** Nginx costs CPU on a one-core box, and its
`worker_connections 16384` caps out around 16k users. Serving direct, every user holds a keep-alive
connection, so the 65,535 `LimitNOFILE` caps concurrent connections. Instead of failing `accept()`
with EMFILE past that point, `IdleConnections` does what nginx does when it runs out of
connections: once open connections reach the limit (minus 1,024 for the JVM's own files), each new
connection closes the keep-alive connection that has been idle the longest. A connection with a
request in flight is never closed, and below the limit nothing is ever closed. Tested with the
limit lowered to 10k connections and 20k users: 11 failed requests out of 366k.

**Lean Ktor.** No plugins. One interceptor routes by path and method and answers with
`respondBytes`. Error bodies are constant byte arrays. Netty's leak detector is off. The write
timeout is 0 (responses are a few KB), so its pass-through handler is removed from each pipeline.

**JWT verified by hand** on every request: `javax.crypto.Mac` HmacSHA256 (one initialized instance
per thread, reused), constant-time compare, HS256 only, `exp`/`nbf` checked. No caching (rule 5).

**JVM.** Serial GC (one core: no GC threads to compete with), an 850 MB fixed heap with a 256 MB
young generation (a connection holds about 3.8 KB of heap, so 64k connections need room in the old
generation), compact object headers. ZGC gave a far lower p99 under connection churn but used 28%
more CPU, and the thresholds (p99 < 1 s) leave room for Serial's pauses.

## License

MIT, under the repo's [license](../../LICENSE).
