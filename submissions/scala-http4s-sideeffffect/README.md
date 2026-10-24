# Scala 3 + Cats Effect + http4s Ember + doobie (SQLite)

An ordinary, idiomatic Typelevel service: http4s routes, a doobie repository, circe JSON. The tuning is in how the
pieces are configured, not in hand-written replacements for them.

| | |
|---|---|
| Language | Scala 3.10.0 on Temurin JDK 25.0.4.1 (official tarball, checksum-verified) |
| Build | sbt 1.13.0 (official tarball, checksum-verified) |
| Effect / streaming | Cats Effect 3.7.1, fs2 3.14.0 |
| HTTP server | http4s 0.23.38, Ember backend, `http4s-dsl` routes |
| Database | doobie 1.0.0-RC12, HikariCP 7.1.0, sqlite-jdbc 3.53.4.0 (bundles SQLite 3.53.4) |
| JSON / JWT | circe 0.14.17 (through `http4s-circe`); HS256 verified with the JDK's `Mac` |
| **Nginx or direct** | **Nginx**: listens on `HOST:PORT` (`127.0.0.1:3000`) behind [`bench/nginx.conf`](../../bench/nginx.conf) |

## Running it

```bash
sudo bash install.sh   # JDK 25 and sbt tarballs into /opt
bash build.sh          # sbt compile, writes target/classpath.txt
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Code

| File | |
|---|---|
| `Main.scala` | `IOApp`, the Ember server |
| `Routes.scala` | the five endpoints and the error bodies, with `http4s-dsl` |
| `Repository.scala` | the SQL, as doobie `ConnectionIO` programs (the reference queries from SPEC.md) |
| `Database.scala` | the transactors: a read pool and a single writer |
| `Jwt.scala` | HS256 verification, every request (no cache, rule 5) |
| `ListenBacklog.scala` | raises the listen backlog (see below) |

## Why Nginx

Ember keeps about 35 KB of heap per open connection: fs2's 8 KiB socket read buffer, and about ten fibers and twenty
fs2 scopes for the connection's stream. Serving directly, every user holds an idle keep-alive connection, so 25,000
users need about 900 MB of live heap on a 2 GB box, and the server spends its time in full GCs. Behind Nginx the app
has a few hundred connections, Nginx closes idle client connections itself when its 16,384 slots run out, and the app
is limited by CPU only. Locally (one pinned core for the app *and* Nginx), direct mode needed a 2 GB heap to hold
20,000 users, while Nginx mode held 30,000 in a 512 MB heap (p95 348 ms, p99 646 ms, 0 errors); 36,000 failed.

## Optimizations, and why

Profiled with async-profiler under `bench/load.js`, the app and Nginx pinned to one core. Each change below came from
a profile.

- **SQLite calls run on the compute pool, not as `blocking`.** doobie wraps every JDBC call (each `setLong`,
  `next`, `getString`) in `Sync.blocking`, because a JDBC call usually waits on the network. SQLite runs in-process
  on pages that are in the cache or `mmap`, so its calls are short CPU work. A custom `KleisliInterpreter` runs them
  as `delay`, and taking and returning a pooled connection is `delay` too (doobie's `Resource.fromAutoCloseable`
  closes it with `blocking`). Each `blocking` is a thread hand-off: removing the last one per request took CPU at the
  same load from 94% to 66%.
- **Requests wait for a connection as fibers.** A `Semaphore` in front of the read pool and a `Mutex` in front of the
  single write connection. Hikari's `getConnection` blocks its thread while the pool is empty; with one compute thread,
  that is the thread the connection holders need to finish, so it deadlocked until Hikari's 30 s timeout.
- **One Cats Effect worker thread per CPU.** The default is at least two. On one vCPU the two workers kept waking
  each other up to steal work: 66% to 21% CPU at the same load.
- **A listen backlog of 4,096 instead of 50.** fs2 binds with Java's default backlog of 50, and neither fs2 nor Ember
  has a setting for it. Nginx opens upstream connections in bursts, the accept queue overflowed (976 overflows in one
  run), and every dropped handshake came back as a 1–3 s retransmit. On Linux, calling `listen()` again on a listening
  socket changes its backlog, so `ListenBacklog` finds the socket among the process's descriptors and does that
  through the JDK's foreign function API. The kernel caps it at `net.core.somaxconn` (4,096 on Ubuntu 24.04).
- **One writer, a few readers.** WAL mode: readers never wait for the writer, and with one write connection, writes
  never wait on SQLite's lock. Every request runs one statement in autocommit mode, so the transactors use
  `Strategy.void`: no `setAutoCommit`/`commit` round trips.
- **Likes in one statement.** `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING` (foreign keys aren't
  enforced, so the existence check has to be in the SQL). The separate existence check runs only when nothing was
  inserted, to tell "already liked" from "no such post".
- **Pragmas** (applied by sqlite-jdbc on each new connection): `journal_mode=WAL`, `synchronous=NORMAL` (rule 6: a
  201 means the row is committed), 1 GiB `mmap_size`, 16 MiB page cache per connection.
- **Ember**: idle connections stay open (Nginx closes its upstream connections after 60 s, never the app first), and
  an 8 KiB read buffer instead of 256 KiB per connection.
- **JVM**: Serial GC (one vCPU: a parallel or concurrent collector only adds threads competing with the app), a
  fixed 512 MB heap that is mostly young generation (behind Nginx almost nothing outlives a request), and Cats
  Effect fiber tracing off (it keeps a ring buffer of events in every fiber).

Not done, to keep the code ordinary: statement caching (doobie prepares each statement per request; preparing is about
15% of the app's CPU), and replacing Ember, doobie or circe with hand-written code.

## License

MIT, under the repo's [license](../../LICENSE).
