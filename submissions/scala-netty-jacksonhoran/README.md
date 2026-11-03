# Scala 3 + Netty (raw HTTP, SQLite through the JDK's foreign function API)

| | |
|---|---|
| Language | Scala 3.9.0 on Temurin JDK 25.0.4.1 (official tarball, checksum-verified) |
| Build | sbt 1.13.0 (official tarball, checksum-verified) |
| Framework | Netty 4.2.18.Final (epoll transport) with a small hand-written HTTP/1.1 layer on top |
| SQLite driver | none: SQLite 3.53.4 compiled from the official amalgamation by `build.sh`, called directly through `java.lang.foreign` (no JDBC, no JNI glue) |
| JSON / JWT | hand-written, using the JDK's `Mac` (HmacSHA256) and `Base64` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: gcc, curl, unzip; JDK 25 and sbt tarballs into /opt
bash build.sh          # compiles native/libsqlite3.so and the Scala app
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **One thread does everything.** A single Netty event loop accepts connections, parses requests,
  runs SQLite and writes responses. The box has one vCPU, so more threads would only add context
  switches. With one SQLite connection used from one thread, writes never wait on a lock.
- **Direct instead of Nginx.** Nginx would take its 14–20% of the only core, and its
  `worker_connections 16384` caps out around 16k users. Netty's epoll transport holds tens of
  thousands of idle keep-alive connections cheaply. In a local test (1 pinned core, 2 GB) it held
  50,000 idle connections open while serving 8,000 req/s of the load-test mix, with 0 errors and
  under 800 MB RSS.
- **Idle connections are closed only under fd pressure.** Keep-alive connections normally stay open
  indefinitely. If open connections near the process's file-descriptor limit (`LimitNOFILE` minus
  1,024 reserved, so 64,511 on the benchmark box), the longest-idle connections are closed: only ones
  idle for over a second, down to 2% below the threshold. A client then reconnects on its next request.
  Without this, the server would fail to `accept()` new connections once it ran out of descriptors.
  Connections are tracked in an O(1) least-recently-active list. In a local test with the server
  capped at 2,048 fds, `bench/load.js` with 3,000 users ran with 0 failed requests (p99 15 ms).
- **Raw HTTP on Netty, not a framework.** The handler parses the request line and only the headers it
  needs (`Content-Length`, `Transfer-Encoding`, `Connection`, `Authorization`, `Expect`) straight from
  the bytes. Keep-alive, pipelining, chunked bodies and `100-continue` all work. The parse buffers are
  shared on the one thread, so an idle connection costs just its Netty channel and a tiny handler object.
- **SQLite without JDBC.** The JDK 22+ foreign function API calls `libsqlite3.so` directly.
  `sqlite3_column_text` pointers are copied straight into the response buffer. There are no `String`
  objects or `ResultSet` layers between SQLite and the socket. The calls use `Linker.Option.critical`
  because none of them calls back into Java.
- **SQLite compiled for this workload**: `SQLITE_THREADSAFE=0` (one thread, so no mutexes) and the
  other [recommended options](https://sqlite.org/compile.html#recommended_compile_time_options).
- **Prepared statements, reused.** Every request still runs its queries while it is served (rule 5).
  These are the reference queries. A like is a single
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. The post-existence check runs
  only when that inserted nothing, to tell "already liked" from "no such post".
- **Responses built as bytes.** Pre-encoded JSON fragments plus column bytes go into one reusable
  buffer, with escaping only when a byte needs it.
- **JWT verified by hand on every request**: HS256 only, constant-time signature compare, `exp`/`nbf`
  checked. No verification cache (rule 5).
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 64 MiB page
  cache. Autocommit mode commits each write before its response is sent.
- **JVM**: Serial GC, because a parallel or concurrent collector would just compete with the event
  loop for the one core. The heap is a fixed, pre-touched 512 MB. Objects are promoted after 2 young
  GCs, so long-lived connection objects stop being copied between survivor spaces.

## License

MIT, under the repo's [license](../../LICENSE).
