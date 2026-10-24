# Scala 3 + Cats Effect + http4s Ember + doobie (SQLite)

An ordinary, idiomatic Typelevel service: http4s routes, a doobie repository, circe JSON. The tuning is in how the
pieces are configured and composed (a Cats Effect `Queue` as the connection pool, a background fiber for checkpoints),
not in hand-written replacements for them.

| | |
|---|---|
| Language | Scala 3.10.0 on Temurin JDK 27 (the newest OpenJDK release; official tarball, checksum-verified) |
| Build | sbt 2.0.11 (official tarball, checksum-verified) |
| Effect / streaming | Cats Effect 3.7.1, fs2 3.14.0 |
| HTTP server | http4s 0.23.38, Ember backend, `http4s-dsl` routes |
| Database | doobie 1.0.0-RC12 over sqlite-jdbc 3.53.4.0 (bundles SQLite 3.53.4) |
| JSON / JWT | circe 0.14.17 (through `http4s-circe`); HS256 verified with the JDK's `Mac` |
| GC / heap | Parallel GC, 60% of RAM, pre-touched |
| **Nginx or direct** | **Nginx**: listens on `HOST:PORT` (`127.0.0.1:3000`) behind [`bench/nginx.conf`](../../bench/nginx.conf) |

**[FINDINGS.md](FINDINGS.md)** has every decision with its measurements, and the ideas that were rejected.
**[RESEARCH.md](RESEARCH.md)** is the survey of the other submissions (merged and open PRs) it started from.

## Running it

```bash
sudo bash install.sh   # JDK 27 and sbt tarballs into /opt
bash build.sh          # sbt compile, writes target/classpath.txt
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Code

| File | |
|---|---|
| `Main.scala` | `IOApp`, the Ember server |
| `Routes.scala` | the five endpoints and the error bodies, with `http4s-dsl` |
| `Repository.scala` | the SQL, as doobie `ConnectionIO` programs (the reference queries from SPEC.md) |
| `Database.scala` | the transactors (a read pool and a single writer) and the background checkpointer |
| `ConnectionPool.scala` | a fixed set of connections lent from a Cats Effect `Queue` |
| `Jdbc.scala` | a connection that prepares each SQL string once (Scala 3 `export` does the delegation) |
| `Jwt.scala` | HS256 verification, every request (no cache, rule 5) |
| `ListenBacklog.scala` | raises the listen backlog from Java's default of 50 |

## The design in short

- **Nginx in front.** Ember keeps about 35 KB of heap per open connection; behind Nginx the app has a few hundred
  connections and is limited by CPU only.
- **One Cats Effect worker thread** (one vCPU), and **SQLite calls as `delay`, not `blocking`**: SQLite works on
  pages in memory, so its calls are short CPU work, and a thread hand-off per JDBC call cost more than the call.
- **A `Queue` of connections as the pool**: a request that finds it empty waits as a fiber; each connection keeps its
  **prepared statements** for the life of the process.
- **WAL with one writer and a few readers**; checkpoints run in a **background fiber** on a blocking thread, so their
  fsyncs never stall a request.
- **Parallel GC with a fixed heap of 60% of RAM**, chosen by measurement against Serial, G1, ZGC and Shenandoah.

Locally (app and Nginx pinned to one physical core, the box's 1,967 MB emulated with a cgroup limit, k6 on the other
cores) it holds **32,000 users** (p95 147 ms, p99 688 ms, 0 errors, RSS 1.36 GB); 36,000 fail on latency.

## License

MIT, under the repo's [license](../../LICENSE).
