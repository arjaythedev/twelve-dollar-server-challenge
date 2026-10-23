# Kotlin, no framework (java.nio + sqlite-jdbc)

| | |
|---|---|
| Language | Kotlin 2.4.21 on the JVM (Temurin JDK 25.0.4.1, HotSpot C2) |
| Framework | none: one `java.nio` epoll selector and a hand-written HTTP/1.1 parser |
| SQLite driver | `org.xerial:sqlite-jdbc` 3.53.4.0 (bundled SQLite 3.53.4) |
| Nginx or direct | **Direct.** Serves `HOST:PORT` itself (use `0.0.0.0:80` on the box). It works behind the Nginx config too |
| Code | ~880 lines across `Server.kt`, `Db.kt`, `Jwt.kt`, `Json.kt`, `Main.kt` |

## Design

All request handling runs on one thread per process, which suits a 1-vCPU box: there's no handoff between threads and no locks.

1. **Single event loop.** One `Selector` (epoll) accepts connections, reads requests, runs the reads against SQLite inline and writes the responses. There's one shared 64 KB read buffer and one shared 1 MB direct output buffer. A connection only gets its own buffer when a request arrives split across reads or the socket won't take the whole response.
2. **SQLite builds the JSON.** `json_object(...)` creates each post object and the feed array is assembled with `group_concat(... ORDER BY ...)`. The driver returns the result as raw UTF-8 bytes (`getBytes`), so a response never becomes a Kotlin object, a `String` or serializer output. The output matches `test/golden` byte for byte.
3. **One connection, statements prepared once** (reusing prepared statements is allowed by rule 5).
4. **Group commit.** `POST /posts` and `POST /posts/:id/like` are queued. After each pass over the ready sockets, the loop runs `BEGIN IMMEDIATE`, all the queued writes and one `COMMIT`. Only then does it send their 201/200s. If the commit fails, every request in that batch gets a 500. A connection with a write in flight doesn't parse its next pipelined request until that write is answered, so responses stay in order.
5. **Checkpoints off the response path.** `wal_autocheckpoint=0` on the serving connection. A low-priority daemon thread on its own connection runs `PRAGMA wal_checkpoint(PASSIVE)` once a second.
6. **JWT verified on every request, without copies.** HMAC-SHA256 runs over the header bytes in place (reused `Mac`, constant-time compare), then the header and payload get a tiny JSON parse (`alg` must be `HS256`, plus the `exp` and `sub`/`username` checks).
7. **Small JVM.** `-XX:+UseSerialGC -Xmx320m` per worker, compact object headers.
8. **fd exhaustion doesn't spin.** If `accept()` hits EMFILE (`LimitNOFILE=65535`), the loop stops polling the listen socket and resumes as soon as any connection closes.
9. **Two worker processes past the 65,535-fd limit.** `RLIMIT_NOFILE` applies per process, so one process can never hold more than ~65.5k sockets. `start.sh` starts `WORKERS` (default 2) identical processes, which share the port through `SO_REUSEPORT`. The kernel spreads new connections across them, so together they can hold ~130k. Only worker 0 runs checkpoints, and `journal_size_limit` caps the WAL file at 64 MB. Set `WORKERS=1` for a single process.
10. **Writes never block the event loop.** Each worker writes through its own second SQLite connection with `busy_timeout=0`. If the other worker holds the write lock, `BEGIN IMMEDIATE` fails at once and the batch stays queued (new writes join it) while the loop keeps serving reads, retrying about 1 ms later. With the default sleeping busy handler, a busy lock froze a worker's whole event loop for 0.5–1.4 s at a time (measured with 2 workers at 120k users).

## Durability trade-off

The database runs in WAL mode with `synchronous=NORMAL`, the minimum rule 6 allows. Each 201 is sent after `COMMIT` returns. With NORMAL, though, a commit isn't fsynced, so a power loss (not a process crash) can lose the most recent transactions. Switching to `FULL` means editing one line in `Db.config()`.

## Local numbers (not the droplet)

Measured on an i7-10700K. The server was pinned to one core in a `MemoryMax=2G` cgroup with no swap and `ulimit -n 65535`. k6 (`bench/load.js`) ran on the other cores across several loopback IPs:

| VUs | workers | hold | p95 | p99 | errors | server CPU | RSS |
|---|---|---|---|---|---|---|---|
| 10,000 | 1 | 30 s | 1.5 ms | 3.6 ms | 0 % | 11 % | 164 MB |
| 30,000 | 1 | 45 s | 1.1 ms | 3.5 ms | 0 % | 22 % | 181 MB |
| **64,000** | 1 | **5 min** | **3.1 ms** | **18 ms** | **0 %** (2.39M reqs) | **45 %** | **212 MB** |
| 64,000 at a 40 % CPU quota | 1 | 60 s | 422 ms | 540 ms | 0 % | 40 % | 206 MB |
| 90,000 | 1 | 60 s | 5 ms | 145 ms | **5.3 % (fail)**: stuck at 65,535 fds | – | – |
| **90,000** | **2** | 60 s | **3.5 ms** | **23 ms** | **0 %** (45.4k / 44.7k fds) | 65 % | 294 MB |
| **120,000** | **2** | 60 s | **49 ms** | **315 ms** | **0 %** (59.7k / 60.3k fds) | 89 % | 313 MB |

The 90k rows show the fd limit directly. One process stops at exactly 65,535 descriptors and the extra users get errors, even though latency is fine. Two processes split the connections evenly and pass. A desktop core is much faster than a shared DigitalOcean vCPU, so treat these as relative numbers.

**On the droplet:** this only removes the server-side fd limit. The other limits described in issue #62 belong to the benchmark rig: source ports per load-generator IP, the conntrack table (if it's active), and k6's memory per VU. A submission can't change any of them, because kernel tuning isn't allowed.

### Measured with a low-overhead generator

k6 itself used 35 GB of RAM and 668 s of CPU to drive 120k VUs on this box, which disturbs a local test. So these runs use [kload](https://github.com/3x3xX3N0N/kload) (also proposed in #87), a small Kotlin replay of `bench/load.js`: same users and thresholds, about 3 KB per VU. Near saturation it reported more tail latency than k6, and an independent `curl` probe running alongside confirmed that the server really was stalling. All runs use the current build, a 60 s hold, one core and `ulimit -n 65535`:

| VUs | workers | p95 | p99 (whole run / hold only) | errors | result |
|---|---|---|---|---|---|
| 64,000 | 1 | 0.3 ms | 0.9 / 0.5 ms | 0 % | pass |
| 64,000 | 2 | 0.5 ms | 1.3 / 1.0 ms | 0 % | pass |
| 90,000 | 2 | 5 ms | 510 / 676 ms | 0 % | pass |
| 100,000 | 2 | 53 ms | 848 / 987 ms | 0 % | pass, barely |
| 120,000 | 2 | 578 ms | 1057 / 1106 ms | 0 % | fail |

Above ~100k the limit is two processes sharing one SQLite file, not CPU: the WAL rarely gets a moment with no reader in either process, so it can't reset. At 64k and below, the second worker costs a few tenths of a millisecond.

Profile at 64k (JFR): about 59 % of the busy time is inside SQLite `step` (mostly the feed's 20 like counts), about 17 % is socket read/write syscalls, and the Kotlin code is a few percent.
