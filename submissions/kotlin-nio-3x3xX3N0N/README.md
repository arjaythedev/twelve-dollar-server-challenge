# Kotlin, no framework (java.nio + sqlite-jdbc)

| | |
|---|---|
| Language | Kotlin 2.4.21 on the JVM (Temurin JDK 25.0.4.1, HotSpot C2) |
| Framework | none: one `java.nio` epoll selector and a hand-written HTTP/1.1 parser |
| SQLite driver | `org.xerial:sqlite-jdbc` 3.53.4.0 (bundled SQLite 3.53.4) |
| Nginx or direct | **Direct.** Serves `HOST:PORT` itself (use `0.0.0.0:80` on the box). It works behind the Nginx config too |
| Code | ~1,080 lines across `Server.kt`, `Db.kt`, `Checkpointer.kt`, `Jwt.kt`, `Json.kt`, `Main.kt` |

## Design

All request handling runs on one thread per process, which suits a 1-vCPU box: there's no handoff between threads and no locks.

1. **Single event loop.** One `Selector` (epoll) accepts connections, reads requests, runs the reads against SQLite inline and writes the responses. There's one shared 64 KB read buffer and one shared 1 MB direct output buffer. A connection only gets its own buffer when a request arrives split across reads or the socket won't take the whole response.
2. **SQLite builds the JSON.** `json_object(...)` creates each post object, and the driver returns it as raw UTF-8 bytes (`getBytes`), so a response never becomes a Kotlin object, a `String` or serializer output. The output matches `test/golden` byte for byte.
3. **Two-step `/feed`.** First the 20 newest ids (index-only on `posts_created_at_id_idx`). When they sit within 256 ids of each other, which is always the case once anybody posts, one range scan of `likes_post_id_idx` is tallied into an array and one rowid range over `posts JOIN users` fetches the 20 post objects; the server puts them back in feed order and appends each `like_count`. Otherwise, or if a row is missing, it runs the original single query with 20 correlated `count(*)`s. Both paths return identical bytes. The range-scan idea is credited to zig-iouring-unorig-username (MIT).
4. **Statements prepared once** (reusing prepared statements is allowed by rule 5).
5. **Group commit with a 2 ms window.** `POST /posts` and `POST /posts/:id/like` are queued. When the oldest queued write is 2 ms old (or 64 are queued), the loop runs `BEGIN IMMEDIATE`, all the queued writes and one `COMMIT`. Only then does it send their 201/200s. If the commit fails, every request in that batch gets a 500. A connection with a write in flight doesn't parse its next pipelined request until that write is answered, so responses stay in order.
6. **A checkpointer that keeps the WAL short.** `wal_autocheckpoint=0` on the serving connections. A thread on its own connection (worker 0 only) runs a `PASSIVE` checkpoint every 50 ms. Once the WAL holds 4,096 frames and all but 128 are copied, it runs a `RESTART` checkpoint with a 250 ms busy timeout. That takes SQLite's WAL write lock, so it briefly holds the writers of every worker (their `BEGIN` fails at once and retries about 1 ms later, while reads go on), copies the short tail, and lets the next write start the WAL from frame 1. The thread lowers its own priority to nice 5 through FFM `setpriority(PRIO_PROCESS, gettid())`, and its connection never commits. The pattern is credited to swift-iouring-akbashev and rust-iouring-kensac (MIT). Without it, the WAL never reset with two processes reading, it grew to hundreds of MB, and every page lookup got slower (in isolation: feed 27 → 45 µs, post 4.7 → 8.7 µs).
7. **No mmap, a 64 MiB page cache** per connection (`mmap_size=0`). This measured 4–10 % less CPU per request than a 512 MB mmap.
8. **Loop pacing.** After a pass with fewer than 32 ready sockets and nothing queued, the loop parks for 1 ms, so the next `select` gathers more sockets per syscall. Under load the passes are heavy and it never parks.
9. **JWT verified on every request, without copies.** HMAC-SHA256 runs over the header bytes in place (reused `Mac`, constant-time compare), then the header and payload get a tiny JSON parse (`alg` must be `HS256`; `exp` must be present, numeric and in the future; then the `sub`/`username` checks). `test-jwt.sh` checks the cases `test/test.sh` doesn't cover (no `exp`, `null`, a string, past, future).
10. **Small JVM.** `-XX:+UseSerialGC -Xmx320m` per worker, compact object headers.
11. **fd exhaustion doesn't spin.** If `accept()` hits EMFILE (`LimitNOFILE=65535`), the loop stops polling the listen socket and resumes as soon as any connection closes.
12. **Three worker processes past the 65,535-fd limit.** `RLIMIT_NOFILE` applies per process, so one process can never hold more than ~65.5k sockets. `start.sh` starts `WORKERS` (default 3) identical processes, which share the port through `SO_REUSEPORT`. The kernel spreads new connections across them, so together they can hold ~195k. Only worker 0 runs checkpoints, and `journal_size_limit` caps the WAL file at 64 MB. Set `WORKERS=1` for a single process. Every extra JVM costs CPU: at 192k users, 4 workers used 46 µs/req against 42 µs/req for 3.
13. **Writes never block the event loop.** Each worker writes through its own second SQLite connection with `busy_timeout=0`. If the other worker holds the write lock, `BEGIN IMMEDIATE` fails at once and the batch stays queued (new writes join it) while the loop keeps serving reads, retrying about 1 ms later. With the default sleeping busy handler, a busy lock froze a worker's whole event loop for 0.5–1.4 s at a time (measured with 2 workers at 120k users).

## Durability trade-off

The database runs in WAL mode with `synchronous=NORMAL`, the minimum rule 6 allows. Each 201 is sent after `COMMIT` returns. With NORMAL, though, a commit isn't fsynced, so a power loss (not a process crash) can lose the most recent transactions. Switching to `FULL` means editing one line in `Db.config()`.

## Local numbers, v2 (2026-11-09)

All numbers are **LOCAL** and relative. The rig is an i7-10700K with the server pinned to one core (CPU 15) in a `MemoryMax=2G` cgroup with no swap and `ulimit -n 65535`. [kload](https://github.com/3x3xX3N0N/kload) ran on the other cores, except CPU 7 (CPU 15's hyper-thread sibling). Each run had a 30 s ramp and a 60 s hold unless noted. µs/req is the server's CPU time (all workers) over the hold divided by the requests in the hold. On this rig v1 passed 112k and failed 120k, while the scored box ran it to 120k at 79 µs/req. So this desktop core does about 1.45× less CPU work per request than the scored box, but its user ceiling came out similar. Don't read the numbers below as a score.

| Step | Change | µs/req (at users) | Max users passing (60 s) | p99 there | cgroup peak there |
|---|---|---|---|---|---|
| v1 | as scored (120k) | 54.7 (112k), 53.3 (128k) | 112k (120k fails, p99 1.01 s at 67 % CPU) | 823 ms | 1,552 MB |
| 1 | reject JWTs without `exp` | 53.7 (112k) | 112k | 872 ms | 1,547 MB |
| 2 | two-step `/feed` | 47.8 (112k), 45.5 (128k) | 128k, barely | 979 ms | 1,711 MB |
| 3 | `mmap_size=0`, 64 MiB cache | 41.7 (128k); A/B at 112k −10 % | 112k (128k fails on p99 1.07 s at 56 % CPU: still the WAL) | 963 ms | 1,556 MB |
| 4 | checkpointer: short WAL, bounded hold, nice 5 | 44.8 (128k) | 128k (fd-capped with 2 workers) | **3.3 ms** | **806 MB** |
| 5 | 2 ms write window + 1 ms park after a light pass | 42.7 (128k); A/B −12 % | 128k (fd-capped) | 5.5 ms | 804 MB |
| 6 | `WORKERS=3` | 42.2 (192k) | **192k** (fd-capped: 64.3k fds in the fullest worker) | 75 ms | 1,201 MB |

**Five-minute hold at 192,000 users (60 s ramp): PASS.** 6.91M requests, 0 errors, p95 4.1 ms, p99 15.7 ms, 85 % CPU, 42.1 µs/req, RSS 503 MB (205 MB idle for 3 JVMs), cgroup peak 1,234 MB (420 MB anon). The checkpointer restarted the WAL 327 times, with the WAL never above 4,389 frames and the longest write hold at 67 ms.

With a 50 % CPU quota as a stand-in for a shared droplet vCPU, at 96k users v1 fails (p99 1,004 ms) and v2 passes (p99 375 ms). At 64k both pass.

Past 192k: 3 workers run out of descriptors at about 195k. 4 workers held 192k at 94 % CPU and failed 224k at 100 % CPU, so the CPU limit on this core sits a little above 200k.

Tried and dropped (measured in isolation, no gain): `last_insert_rowid()` instead of `RETURNING` for new posts, and a single-statement `INSERT … SELECT … WHERE EXISTS` like.

## Local numbers, v1 (before the changes above)

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

Above ~100k the limit is two processes sharing one SQLite file, not CPU: the WAL rarely gets a moment with no reader in either process, so it can't reset. (v2's checkpointer fixes this by holding writes for a few ms so the WAL can restart.) At 64k and below, the second worker costs a few tenths of a millisecond.

Profile at 64k (JFR): about 59 % of the busy time is inside SQLite `step` (mostly the feed's 20 like counts, which v2's two-step feed replaces), about 17 % is socket read/write syscalls, and the Kotlin code is a few percent.
