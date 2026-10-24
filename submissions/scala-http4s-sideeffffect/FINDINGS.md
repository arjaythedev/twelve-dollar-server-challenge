# Findings and decisions

How this submission got its shape: what was measured, what was adopted, and what was rejected and why. The survey of
the other submissions that many of these ideas came from is in [RESEARCH.md](RESEARCH.md).

The goal throughout: **idiomatic Typelevel Scala that is still competitive**. A change is adopted when it is measured
to help *and* it reads like ordinary Cats Effect / fs2 / doobie / http4s code. Replacing the stack (hand-written HTTP
parsing, raw Netty, FFM bindings to a self-compiled SQLite, SQL-built JSON) is out of scope, even though the fastest
JVM entries do exactly that.

## How it was measured

- `bench/load.js` (k6), against a fresh copy of the seed for every run, after a 30 s warm-up at 500 users.
- The app and Nginx (the official `bench/nginx.conf`, `worker_connections 16384`) pinned to **one CPU**; k6 on other
  CPUs, split across two client IPs (one IP pair has only ~28,000 ephemeral ports, which capped earlier runs).
- The box's **1,967 MB of RAM emulated with a cgroup limit** (`systemd-run --scope -p MemoryMax=1967M`): the JVM sizes
  its heap from it. (`-XX:MaxRAM` can't do this any more: JDK 27 removed it.)
- App CPU from `/proc/<pid>/stat` over a 40 s steady window; profiles with async-profiler (`itimer`).
- Laptop: i7-11800H. Caveat found late: CPUs 8-15 are the hyper-thread siblings of 0-7, and in the comparisons below
  k6 also ran on CPU 8, the sibling of the app's CPU 0. Every configuration had the same handicap, so the comparisons
  hold; their absolute numbers are pessimistic. The final numbers (last section) were measured with CPU 8 left idle.

## Front: Nginx, not direct

| | Direct | Behind Nginx |
|---|---|---|
| Heap per open connection | ~35 KB (fs2's 8 KiB socket buffer + ~10 fibers + ~20 fs2 scopes, from a heap histogram) | a few hundred connections in total |
| Users held locally | ~20,000, and only with a 2 GB heap (thrashing in full GCs beyond) | 30,000+ with a 1.2 GB heap |

Every load-test user keeps a keep-alive connection, so direct mode is memory-bound long before it is CPU-bound. Nginx
closes idle client connections itself when its slots run out, so the app's connection count stays small. Nginx costs
CPU (14-20% in the video), but less than the GC pressure of tens of thousands of Ember connections.

Considered, not done: **http4s-netty** as the backend (Netty-based JVM entries report ~4 KB per connection, which
would make direct mode viable). The brief was Ember; it is the obvious next experiment.

## Changes, in the order they were found

| Change | Measured effect (same load) |
|---|---|
| SQLite JDBC calls as `delay` on the compute pool (custom doobie `KleisliInterpreter`), not doobie's default `blocking` | part of 94% → 66% CPU |
| Connection checkout and return without `blocking` (doobie's `Resource.fromAutoCloseable` closes with `blocking`) | 94% → 66% CPU |
| One Cats Effect worker thread instead of the default minimum of two | 66% → 21% CPU: the two workers on one vCPU spent most of their time waking each other to steal work |
| Requests wait for a connection as fibers, never in Hikari's blocking `getConnection` | fixed a deadlock: with one thread, a blocked `getConnection` stops the very fibers that would return a connection (30 s stalls) |
| Ember read buffer 8 KiB instead of 256 KiB per connection | an `OutOfMemoryError` at 10,000 direct connections went away |
| Fiber tracing off (`-Dcats.effect.tracing.mode=none`) | ~800 B less per fiber, ~10 fibers per connection |
| **Listen backlog 4,096 instead of 50**, raised with `listen()` through the JDK's FFM API (fs2 binds with Java's default and has no setting) | 976 → 0 accept-queue overflows per run behind Nginx, and no more 1-3 s SYN-retransmit tails |
| Nginx front (above) | 20,000 → 30,000+ users |
| **Prepared statements reused**: a Cats Effect `Queue` of fixed connections replaces HikariCP, and each connection keeps one `PreparedStatement` per SQL string (`Jdbc.scala`, delegation by Scala 3 `export`) | preparing went from ~15% of app CPU to ~1% |
| **`jdbc.get_generated_keys=false`**: sqlite-jdbc otherwise runs `SELECT last_insert_rowid()` after every INSERT (read in its source) | one statement less per write |
| **Checkpoints in a background fiber** (`wal_autocheckpoint=0`, `PRAGMA wal_checkpoint(PASSIVE)` every second through doobie's default, `blocking` interpreter on its own connection) | the write that crossed 1,000 WAL pages no longer does two fsyncs on the only worker thread |
| The three changes above together | app CPU 50% → 41% (−18%); p95 338 → 5 ms, p99 726 → 203 ms (25,000 users, Serial GC, same harness) |

## JVM: version, GC, heap

- **OpenJDK 27** (Temurin), the newest release. Compact object headers are on by default in 27, so that research
  idea came for free.
- **GC: Parallel.** All five collectors on the same build and load:

| GC | 25,000 users: app CPU, p95 / p99 | 32,000 users: app CPU, p95 / p99, errors |
|---|---|---|
| **Parallel** | 41%, 15 ms / 655 ms | **50%, 187 ms / 841 ms, 0% (passes)** |
| Serial | 41%, 100 ms / 754 ms | 51%, 286 ms / 1.36 s (fails p99) |
| G1 | 43%, 55 ms / 467 ms | 54%, 576 ms / 2.82 s (fails) |
| ZGC (generational) | 49%, 673 ms / 2 s | not run (out at 25k) |
| Shenandoah (generational) | 47%, 9 ms / 104 ms | 68%, 26 s / 36 s, 8.8% errors (collapses) |

  On one vCPU the concurrent collectors (ZGC, Shenandoah) take CPU from the app exactly when it is short of it:
  Shenandoah has the best tail at moderate load and collapses near the ceiling. Parallel GC runs one stop-the-world
  thread here, like Serial, but sizes the young generation adaptively, and was the only one to hold 32,000 users.
- **Heap: 60% of RAM, fixed and pre-touched** (`-XX:MaxRAMPercentage=60 -XX:InitialRAMPercentage=60
  -XX:+AlwaysPreTouch`): about 1.18 GB of the box's 1,967 MB, RSS about 1.4 GB. The rest is for the OS, Nginx and the
  page cache that holds the 240 MB database. There is no swap and the heap never grows after start, so if it starts,
  it fits. A bigger heap would starve the page cache; behind Nginx the live set is small, and the extra heap only
  makes young collections rarer.

## Robustness (from an independent code review)

| Finding | Fix |
|---|---|
| sqlite-jdbc finalizes a statement itself after most errors (I/O, disk full) without marking it closed; the statement cache would have kept handing out the dead statement, so one transient error on the single write connection broke every later write | `ReusablePreparedStatement.close()` notices (clearing the parameters of a finalized statement throws), and the connection prepares the SQL again next time |
| The checkpointer swallowed its errors, ran through the statement cache, and `wal_autocheckpoint=0` left no backstop: a failing checkpointer would have let the WAL grow without bound | Failures are reported to stderr and retried every second, on a plain connection; SQLite's own autocheckpoint stays on as a backstop at 10,000 pages (~40 MB) instead of 1,000 |
| `busy_timeout=5000` on the only worker thread | 1,000 ms: nothing should wait for a lock with WAL, one writer and PASSIVE checkpoints, but if something ever does, it stalls every request |
| Waiting for a pooled connection couldn't be cancelled | `Resource.makeFull` with `poll` around the queue's `take` |
| `token.split('.')` drops trailing empty parts, so a token with trailing dots was accepted | `split("\\.", -1)`: exactly three parts, or 401 |

## Final numbers

The final code (all of the above, including the robustness fixes), app and Nginx on physical core 0 with its
hyper-thread sibling idle, k6 on the other cores, nothing else running:

| Users | Result | app CPU | p95 / p99 | errors | RSS | WAL at the end |
|---|---|---|---|---|---|---|
| 28,000 | pass | 42% | 5 ms / 151 ms | 0 | 1.28 GB | 8 MB |
| **32,000** | **pass** | 48% | 147 ms / 688 ms | 0 | 1.36 GB | 10 MB |
| 36,000 | fail (p99) | 53% | 672 ms / 2.33 s | 0 | 1.36 GB | 12 MB |

The WAL staying at 8-12 MB shows the background checkpoints keep up: the WAL restarts from the beginning, and the
10,000-page backstop (~40 MB) never fires. App CPU stays around 50% because Nginx shares the core.

## Considered and not adopted

| Idea | Why not |
|---|---|
| Oracle GraalVM (Graal JIT; one entry measured 6-20% less CPU than C2) | The brief is the newest stable OpenJDK. Worth trying as a drop-in later. |
| GraalVM native-image | Needs ~1.8 GB to build (the box has 2 GB), and was slower than Graal JIT in another entry. |
| One SQLite connection for everything, `locking_mode=EXCLUSIVE` (+5% in the Node entry) | Conflicts with the background checkpointer's own connection; the read pool keeps reads independent of a write in progress. |
| Group commit (an fs2 `Queue` of writes, `Deferred` per request) | Writes are ~8% of requests; other entries measured ≤ 3%. Not worth the code. |
| jsoniter-scala instead of circe | circe is ~8% of app CPU in the profile; the gain would be a few percent. circe stays, as the ordinary choice. |
| `query_only` reader connections, pre-warming the JIT before binding | Small or startup-only effects. |
| Closing idle connections under fd pressure (`Connection: close` near the limit) | Only matters when serving directly. |
| Shared read transaction for a batch, timestamps bound by the app, SQL-built JSON, two JVMs on one port | Rule risks flagged in batch 2, or replacing the stack. See RESEARCH.md. |
