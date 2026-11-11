# Java: io_uring, a write queue, and SQLite's B-tree cursors, through FFM

A Java port of [goose-iouring-davidhsv](../goose-iouring-davidhsv) (#384), which ports the design of [rust-iouring-kensac](../rust-iouring-kensac) (#264): one thread serves every connection through io_uring and every request through one SQLite transaction per pass. The question here is what that design costs in Java on HotSpot, with every decision made in Java and C kept to what the JVM cannot do by itself, measured against the entries it comes from.

| | |
|---|---|
| Language | Java 27, OpenJDK 27.0.1 (the GA build from jdk.java.net), fetched by `install.sh` and checked against its published sha256 |
| Framework | none: the event loop, HTTP/1.1, JSON and HS256 (SHA-256 and HMAC included) are written in Java here |
| Native calls | `java.lang.foreign` (FFM, final since Java 22) into `libfeed.so`, built from `net.c` and `db.c` |
| io_uring | raw system calls against the kernel's `<linux/io_uring.h>`; no liburing |
| SQLite | 3.54.1 amalgamation (sha256-checked), unmodified, compiled into `libfeed.so`; opened through a small VFS of its own over `unix-excl` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, link-time and profile-guided optimization |
| JVM | HotSpot, Serial GC, 64 MB heap |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors). No epoll fallback, so it does not start where io_uring is blocked (Docker's default seccomp profile) |

## What is Java and what is C

| Java (`src/`, 2,400 lines) | C (`net.c`, `db.c`, 1,300 lines) |
|---|---|
| the loop: waiting, draining every ring's completions, flushing after COMMIT, re-arming receives | `io_uring_setup`/`enter`/`register`, queueing operations, copying completions to a native array |
| the connection table: generations, flags, what a connection is waiting for (primitive arrays) | receive buffers, pending output, the send in flight, input kept for later |
| accepting: how many in flight, on which ring, when a ring is full, when to add one | rings, sparse direct-descriptor tables, listening sockets |
| the write queue, group commit, the checkpoint policy | the checkpoint thread (it copies when asked) and the log VFS |
| HTTP/1.1 framing and parsing, routing, every status code and error message | |
| HS256: base64url, SHA-256, HMAC (pad states precomputed), the claims | |
| a validating JSON reader (any document, escapes, surrogate pairs, UTF-8) and the JSON writer | |
| JavaScript's `trim()` on the body and its code-point count | |
| SQLite's record format: varints, serial types, column ranges | moving SQLite's B-tree cursors, the address of a row's record |
| the feed and post read plans, and their fallback to SQL | BEGIN/COMMIT, the two INSERTs, `SELECT 1` |

`Server.java` is the loop, the connections, the write queue and the checkpoint policy; `Http.java` the framing; `Api.java` routing, responses, the read plans and the record format; `Json.java`, `Jwt.java`, `Out.java` (the JSON writer) and `Vec.java` (its vector loop); `N.java` the downcalls and native memory; `Buf.java` a growable native buffer. Neither C file decides anything.

## How Java talks to the kernel and to SQLite

- **Downcalls through FFM**, every pointer passed as a `long`: on the 64-bit ABIs a pointer and a long are passed the same way, so no `MemorySegment` is created per call. Each function has a downcall handle built without an address (`Linker.downcallHandle(descriptor, options)`) in a `static final`, and its address in a second, lazily loaded class; the call is `invokeExact` with the exact signature.
- **`Linker.Option.critical(false)`** for everything short that never waits: queueing an operation, moving a cursor, giving a buffer back, a record's address, the transaction's BEGIN. The thread stays in Java state for the call. The waiting `io_uring_enter`, COMMIT, the INSERTs, opening and the checkpoint request are ordinary downcalls, so that the JVM can bring the thread to a safepoint while they run. Measured on x86-64, the downcall stubs (`nep_invoker_blob`, `linkToNative`) are about 0.3 % of the process's samples, so the FFM boundary does not matter here and the read plans stay in Java.
- **Native memory is read in place.** A received message is parsed where the kernel put it (its buffer goes back to the kernel once it is served), input kept for later is parsed in its chunk, and a post's record is decoded on SQLite's page. Java reads all of native memory through one `MemorySegment` (`MemorySegment.NULL.reinterpret(Long.MAX_VALUE)`) at absolute addresses. What is copied: a feed's 20 bodies, timestamps and usernames into a native scratch buffer, because the posts are read in id order and written in feed order and a record's address is dead once its cursor moves (`/posts/:id` writes straight from the pages); and a response's body into the connection's output behind its head, because `Content-Length` comes first. The Goose entry copies the same.
- **No allocation once it runs.** State is in primitive arrays and native buffers, constants are `static final byte[]` and `static final long`, nothing on the request path creates a `String`, a boxed number, a lambda or an exception. Serial GC with a 64 MB heap is the safety net. Evidence, with a 4 MB young generation (`-Xmn4m -Xlog:gc`) at 33,600 req/s: on both GitHub x86-64 runners, before the Vector API was added, the only collections were four in the first 0.44 s of startup; with it, the module's startup takes 20 collections in the first 1.5 s, and then none through the 30 s hold (the 6.8 runner logged one more at 47.2 s, as the run ended and the load generator closed its 40,000 connections). Over a 90 s hold on arm64, 3.03 million requests, there was no collection after 0.89 s, which bounds what the request path allocates below about 1.3 bytes per request. Arrays grow only when a ring is added, or past 4,096 queued writes.
- **The JSON writer** writes a post's constant pieces as whole 8-byte words and copies each string while checking it for bytes that need escaping, in one pass, a vector register at a time with the Vector API (`jdk.incubator.vector`, an incubating JDK module, not a preview feature; 256-bit vectors, never 512, for the box's Cascade Lake). It escapes only when a check finds something.

## A pass

1. Every other ring submits and collects without waiting; a ring with nothing queued and no deferred work pending (the kernel says so in the ring's flags) costs no system call. Ring 0 goes last and waits for 256 completions or 1 ms.
2. Each ring's completions are handled in order. A receive is served at once: every complete request in the message is parsed and answered into the connection's pending output; an incomplete one is kept with the connection.
3. Reads run right away, inside the pass's SQLite transaction. Writes are validated and queued.
4. If the write queue is due, it runs.
5. `COMMIT`, and only then does every touched connection's output go to the kernel: **no response of a pass is sent before its COMMIT returns**. If the commit fails, every answer of the pass becomes a 500 and its connection closes.
6. Receives that ended are armed again, accepts are topped up, and the checkpoint policy runs.

## The write queue

A `POST` is checked when it arrives: the token's signature, `alg`, `exp` and claims, the id, the JSON body, the trim and the length. A request that fails is answered in that pass. One that passes becomes a record in primitive arrays (the connection and its generation, the user id, the post id) plus the username and trimmed body in a native buffer, and its connection reads no further request until that write is answered. The queue runs 5 ms after its first write arrived: all queued writes, in arrival order, in one transaction with one COMMIT (group commit, rule 6). The 404 check of a like and both INSERTs happen when the queue runs. Nothing about the benchmark is detected: every write takes this path.

## Reads

`GET /feed` and `GET /posts/:id` move SQLite's own B-tree cursors inside the pass's transaction, so only the SQL bytecode engine is skipped: pages come through SQLite's pager, WAL and page cache.

- **Feed:** the first 20 entries of `posts_created_at_id_idx` (`created_at DESC, id DESC`, the reference `ORDER BY`), the 20 posts by rowid in ascending order, their authors in `users`, and the like counts: one walk of `likes_post_id_idx` when the ids span at most 512, a seek per post otherwise.
- **Post:** a rowid seek on `posts`, one on `users`, and the like count.
- **Nothing about the data is assumed.** A post whose user row is missing sends the feed to the reference SQL query. The column order of every table and index that is decoded is checked at startup.
- **Only SQLite's B-tree interface is called** (`sqlite3BtreeTableMoveto`, `IndexMoveto`, `First`, `Next`, `PayloadSize`, `PayloadFetch`, `Payload`), and **no SQLite structure is written from outside** (ruling 8). Like counts step the cursor with `sqlite3BtreeNext`.
- Kept between requests: prepared statements, cursor objects, `KeyInfo`, the four root page numbers, and the HMAC key schedule. Nothing read from the database outlives the request that read it.

Writes are SQL: `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`, where `sqlite3_changes` tells a new like from a repeat.

## Durability and checkpoints

- WAL with `synchronous=NORMAL`, opened through SQLite's `unix-excl` VFS (the WAL index is heap memory shared by the process's two connections). 64 MB page cache, no mmap, `wal_autocheckpoint=0`.
- A second thread on its own connection, at nice 19, runs a PASSIVE checkpoint each time the loop asks. Past 4,096 log frames (16 MB) the loop asks for a copy; when it finishes, the write queue is held (reads go on) and the thread copies the rest, so that the next write starts the log over. **A hold lasts at most 50 ms**; then writes go on and a later round tries again. These numbers were chosen on slow disks (below).
- **The loop never calls fsync.** With `synchronous=NORMAL` a commit does not sync; the one sync SQLite asks of the loop's connection is the log header's when a write starts the log over. That sync orders the new header before any later write to the database file (SQLite's ticket ff5be73dee). The database file is written only by checkpoints, on the checkpoint thread, which syncs that header first and whose checkpoint syncs the log again before it copies a frame; so the VFS leaves the header's sync to that thread. On a slow device the loop's header sync took up to 789 ms before this. This ordering argument is from reading SQLite's source; it is not something a test here can show (no test cuts the power).
- **One write per commit**: the "feed" VFS gathers the headers and pages of frames appended at the log's end and writes them in one call when a commit frame's page arrives, inside SQLite's own write call, before COMMIT returns (the idea of goose-iouring-ahmadsh2007 #291). Only appends are held back; everything else writes out what is gathered first.
- `kill -9` tests, each on one database across its rounds, the server killed at a random moment: 16 writers, six kills, 14,116 posts and 14,249 likes acknowledged; 64 writers with a 10-page cache (every transaction spills and rewrites frames), eight kills, 24,912 posts and 24,749 likes. Every acknowledged row was in the database afterwards and `integrity_check` said ok each time. Both ran again after each change to the VFS or the checkpoint policy.

## Slow disks

goose-iouring-davidhsv held 220,000 users on the box and failed 230,000 on latency (p95 421 ms, p99 1.53 s) at 74 % CPU, while rust-iouring-kensac holds 310,000 at the same 25 µs/request. One explanation is the disk: writes are 8 % of requests, and the Goose policy (16,384-frame checkpoint cycles, writes held up to 200 ms, a log-header fsync on the loop) could delay them when fsync is slow. That was tested here, not assumed.

**Setup** (GitHub `ubuntu-22.04` runners, kernel 6.8, the servers in an Ubuntu 24.04 container): the database on ext4 over device-mapper `delay` over a loop device, with read/write/flush delays of 0/0/0, 1/3/10 and 2/10/30 ms, optionally capped at 20 or 40 MB/s of writes by the container's cgroup (`--device-write-bps`); 200 fsync'd 4 KB writes took 0.2 s, 3.8–4.3 s and 12 s on them. Five-minute holds, the one-ring load at 33,600 req/s (60–90 % CPU), per-second `FEED_STATS`. A second series also capped the server's own cgroup at 1,800 MB and held 230,000 connections at the load test's own think times (24,250 req/s); the cgroup peaked at 0.58–1.17 GB with no memory events, so memory was not the limit there.

**What it shows:**

- The setup does not reproduce the box. On every device where kensac stayed healthy, the Goose entry passed too, with write latency no worse than kensac's (POST like p95 37–221 ms against 102–229 ms). On the slowest device kensac was the one that failed (p95 3.1–4.1 s, 26–28k of 33.6k req/s served, reads stalled as well), while the Goose and Java entries passed. So a slow disk alone, as modelled here, does not explain 220,000 against 310,000: kensac cannot serve as a control in a setup that slows it down first.
- What the stats do show for this design on a slow disk: writes held by the checkpoint gate for 60–180 s of each 315 s run, the oldest queued write waiting up to 400–530 ms, checkpoints taking up to 16 s, and the log growing to a million frames when the checkpoint thread cannot keep up. The loop's own log writes stayed under 50 ms except twice (202 and 211 ms), and its header fsync at a log restart took up to 789 ms (once 3.8 s in a Goose commit).

**Policy A/B** (Java, same runner and device, five-minute holds; POST like p99 / overall p99):

| device | 16,384 frames, 200 ms (Goose's, header sync already off the loop) | 4,096 frames, 250 ms (kensac's) | **4,096 frames, 50 ms** |
|---|---:|---:|---:|
| 0/0/0 ms | 188 / 66 ms | 246 / 139 ms | **111 / 78 ms** |
| 1/3/10 ms | 254 / 94 ms | 328 / 172 ms | **90 / 59 ms** |
| 1/3/10 ms, 40 MB/s | 295 / 156 ms | 344 / 164 ms | **86 / 57 ms** |
| 2/10/30 ms, 20 MB/s | 360 / 197 ms | 459 / 254 ms | **98 / 53 ms** |

Hence 4,096-frame cycles, holds of at most 50 ms, and the header sync on the checkpoint thread (removing the loop's fsync made no clear difference in these runs, max pass 38–448 ms with and 115–304 ms without; it removes the 789 ms case by construction). Spreading the database file's writeback with `sync_file_range` (`FEED_WRITEBACK_KB`) was built and passes the tests, but was not measured on the slow disks and is off.

**Highest rate that passes** (p95 < 500 ms, p99 < 1 s, < 1 % errors over five minutes) on the 1/3/10 ms, 40 MB/s device: rust-iouring-kensac passed 33,257 req/s offered at 33,600 (p99 721 ms) and failed at 42,000, 46,200 and 50,400 offered (p99 2.5–3.0 s); this entry passed at every rate up to its CPU limit, 46,428 req/s served at 50,400 offered (p95 123, p99 188 ms; the Goose entry 47,592). Different runner CPUs in each row (EPYC 9V74, 7763).


## Connections

- Accepted sockets are io_uring **direct descriptors**, so `LimitNOFILE=65535` caps a ring, not the server; a ring takes accepts until fewer slots are free than the accepts in flight plus 64, then a ring with room takes over, and one is added when none has room (up to 16). Accepts are single-shot, 512 in flight over eight `SO_REUSEPORT` listeners.
- One **multishot receive** per connection into the ring's provided buffers (16,384 × 1 KB). A receive that ends for lack of a buffer is armed again at the end of the pass. A 3.5 s `SIGSTOP` under 33,600 req/s ended 30,835 receives that way; the server caught up with no error.
- One send in flight per connection; `SHUTDOWN` hard-linked to a direct `CLOSE`; a generation in every `user_data`. Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero (Launchpad #2162843), so a refused one is tried again with `resv[0] = 1`.
- The code of `libfeed.so` and `libjvm.so` is `mlock`ed (19 MB): with no swap, reclaim takes file-backed pages first. The JIT's code is anonymous memory anyway.

## HTTP, JSON, JWT

As in the Goose entry, byte for byte: only the headers the API uses are read; heads over 16 KB, bodies over 1 MB, chunked bodies and malformed request lines get `400 {"error":"bad request"}` and a close; `Connection: close` and HTTP/1.0 close after the response. HS256 only, the signature checked on every request in constant time, only the canonical base64url spelling accepted, `exp` required and in the future, then `sub` a positive integer string and `username` a string. The JSON reader validates whole documents with an explicit stack, decodes escaped member names and lets the last duplicate win. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code points.

Behaviour, request for request, on fresh seed copies (`difftest.py`, 214 raw-request cases: routing, every token failure, 80 JSON bodies with escapes, surrogate pairs, Unicode trimming, multi-byte limits, duplicate keys, nesting, invalid UTF-8, pipelining, byte-by-byte splits, half-closed connections, malformed framing): against goose-iouring-davidhsv, 0 differences; against rust-iouring-kensac, the same four the Goose entry has, by intent (this one closes after `Connection: close` and HTTP/1.0, three cases, and answers 400 to a 17 KB head).

## What differs from the Goose entry

- **The language**: everything that decides is Java, on HotSpot. Where Goose copied received bytes and record bytes into its own memory (its FFI cannot keep a borrowed pointer), Java reads them where they are.
- **Checkpoint policy**: 4,096-frame cycles and holds of at most 50 ms (Goose: 16,384 and 200 ms), and the loop never fsyncs; both measured on slow disks (above).
- **The JSON writer** works a word and a vector at a time (gcc vectorised Goose's checks by itself).
- Disk timings in `FEED_STATS` (the loop's log writes and syncs, checkpoint durations, how long writes were held, the oldest queued write).

## Build

`install.sh` (root, once): `build-essential curl ca-certificates unzip python3`, and OpenJDK 27.0.1 from jdk.java.net into `/opt/jdk-27.0.1`, checked against the published sha256 (x86-64 and aarch64).

`build.sh` (user): fetches and checks SQLite, compiles `src/*.java` (`javac --release 27`, warnings as errors) into `bin/server.jar`, links `bin/server` to the JDK's `java` (so the process is named `server`), and builds `bin/libfeed.so` at `-O3 -flto`, `-fvisibility=hidden` (only the functions in `feed.h` are exported), `-march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256` on x86-64, Ubuntu's default hardening off. **Profile-guided**: an instrumented library serves `train/train.py` first (a synthetic database of its own, tokens of its own, the load test's request mix and the error paths); SIGTERM ends the JVM through `exit()`, which writes the profile; then the library is rebuilt with it. If training fails it is built without a profile; `PGO=0` skips it. SQLite options are the Goose entry's: `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `USE_ALLOCA`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`, `OMIT_COMPLETE`, `OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

`start.sh`: `-XX:+UseSerialGC -Xms64m -Xmx64m -Xss1m -XX:MaxMetaspaceSize=32m -XX:ReservedCodeCacheSize=48m -XX:MaxDirectMemorySize=16m -XX:-UsePerfData -XX:+DisableAttachMechanism --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED`. The JVM prints one warning at startup for the incubating module. It is healthy in under a second.

## Rules

1–3. The SQLite file at `$SQLITE_PATH`, in-process; schema untouched; reads through SQLite's B-tree cursors and pager, writes in SQL.
4. `test/test.sh`: 42/42 (arm64 in a clean Ubuntu 24.04 container as an unprivileged user, and x86-64 on GitHub runners).
5. No caching across requests; scratch buffers are storage only.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for the commit of its pass.
7. Nothing detects the load test.
8–9. Direct on `HOST:PORT`.
10. Built from source on the box; the JDK and SQLite pinned by version and sha256.
11. Nothing on the box is tuned; the process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local measurements (not a score)

Not the organizer's box: GitHub-hosted runners (4 vCPUs), the servers built from their folders by their own `build.sh` and run in an Ubuntu 24.04 container, each pinned to one vCPU on a fresh seed copy. The load is a native replay of `bench/load.js`'s user model (one keep-alive connection per user, the same request mix) on the other three vCPUs over loopback: 15 s ramp, 30 s hold, the servers in turn, round after round. CPU is the server process's user plus system time over the hold (every JVM thread included) divided by the requests it answered. Loopback puts the client on the same machine and there is no network card, so read these as a comparison between entries, not as the box's numbers.

Kernel 6.8.0-1070-azure (the box's series), AMD EPYC 7763, 2 rounds (300,000 connections: 1 round), in the order rust-iouring-kensac / goose-iouring-ahmadsh2007 / goose-iouring-davidhsv / **this entry** / java-nio-stohirov #303:

| | µs/request (user + system) | CPU | p95 | p99 | served req/s |
|---|---|---|---|---|---|
| one ring, 33,600 req/s | 24.23 / 22.84 / 22.56 / **24.46** (7.40 + 17.05) / 24.54 | 81.0 / 76.5 / 75.6 / **81.9** / 82.2 % | 7.7–13.3 / 7.2–8.7 / 7.2–10.2 / **9.2–10.8** / 5.9–6.9 ms | 131–238 / 53–131 / 12–66 / **23–28** / 51–57 ms | all 33,4xx–33,5xx |
| five rings, 33,600 req/s | 26.01 / 26.31 / 26.30 / **28.37** / 27.86 | 86.5 / 87.6 / 87.5 / **93.1** / 92.3 % | 12–27 / 17–24 / 18–20 / **31–197** / 28–33 ms | 66–86 / 32–78 / 26–27 / **45–393** / 51–82 ms | |
| one ring, 54,600 offered (saturated) | 27.09 / 26.20 / 24.75 / **26.05** / 38.27 | 98–99 % | | | 36,307 / 37,453 / 39,894 / **38,034** / 25,902 |
| five rings, 54,600 offered (saturated) | 24.94 / 24.23 / 23.79 / **25.05** / 26.11 | 98–99 % | | | 39,442 / 40,386 / 41,503 / **39,379** / 37,818 |
| 300,000 connections, 31,000 req/s | 27.01 / 24.31 / 25.11 / **26.26** / 26.35 | 83.9 / 75.5 / 78.0 / **81.7** / 81.8 % | 18 / 11 / 15 / **20** / 14 ms | 43 / 22 / 57 / **64** / 21 ms | |

At 300,000 connections the kernel held 2,368 bytes of TCP slab, 832 of inode and 256 of file per socket, as for the Goose entry (about 1.05 GB for 300,000), and this server's resident memory was 292,152 kB (VmRSS sampled during the hold), against 163,536 kB for the Goose entry and 93,056 kB for kensac: about 1.35 GB together, of the box's 1,967 MB. The JVM's share (NMT): 64 MB heap reserved and committed but mostly untouched, 15 MB class-data archive, 6 MB code, 3 MB metaspace, plus 19 MB of `libjvm.so` and `libfeed.so` locked in memory.

On kernel 7.0 (`ubuntu-24.04`, EPYC 9V74), the same order: 18.15 / 17.08 / 16.94 / **18.21** / 18.21 µs at one ring, 18.42 / 17.34 / 17.48 / **18.71** / 18.58 at five rings, saturation 51,282 / 51,970 / 52,021 / **50,269** / 48,863 req/s at one ring, 300,000 connections 20.28 / 18.32 / 18.10 / **19.90** / 20.47 µs.

**So this entry needs 5–8 % more CPU per request than the Goose entry it ports** (24.46 against 22.56 µs and 26.26 against 25.11 on kernel 6.8; 18.21 against 16.94 on 7.0), about what kensac needs, and serves 3–5 % fewer requests at saturation. All of the difference is user time (7.40 against 5.84 µs; system time is equal), and the profile says where: with perf and the JVM's perf map on an x86-64 runner, before the JSON writer was rewritten, building the feed's JSON took 8.4 % of the process's samples in Java against about 3 % for the same work in the Goose entry's compiled C (gcc turns its constant appends into immediate stores and vectorises its escape check by itself), and decoding the feed's records 3.1 %. The FFM boundary itself is not where the time goes (0.3 %). Before the JSON writer was rewritten, the gap measured 20.73 against 17.81 µs (16 %) on an Intel Xeon 8370C runner; the comparisons are on different runner CPUs, so the size of the improvement is not exact. Its p95 is higher at low load for the same reason as the Goose entry's (writes wait up to 5 ms for company), and it showed occasional p99 spikes in the five-ring runs that the other entries did not.

**Tried and measured** (interleaved A/B on the runners, 3 rounds, µs/request at 33,600 req/s, kernel 6.8 then 7.0):

| change | result | kept |
|---|---|---|
| post objects written a word at a time, strings copied and checked in one pass | 20.03 vs 20.26, 22.39 vs 23.23 | yes |
| that copy-and-check a vector at a time (Vector API) | 24.99 vs 25.09 and 24.57 vs 24.85 (warmed), 17.70 vs 18.21 and 17.80 vs 18.10 | yes |
| small copies as 8-byte loops instead of `MemorySegment.copy` | 11.32 vs 10.85, 18.65 vs 18.32 (worse) | no |
| Leyden AOT cache (`-XX:AOTCacheOutput` from the training run, JDK 27) | 11.27 vs 11.32, 18.46 vs 18.65; RSS −10 MB | no (no change) |
| `-XX:-TieredCompilation` | 11.43, 18.49 | no |
| `-XX:+UseTransparentHugePages` | 11.66, 18.80 (worse) | no |
| `-Xshare:off` | 11.25, 18.30 | no (within noise) |
| `-XX:+UsePerfData` (the default) | 11.70, 18.93 (worse) | no: `-UsePerfData` |
| more inlining (`FreqInlineSize=650`, `InlineSmallCode=6000`, `MaxInlineLevel=25`) | 11.82, 18.22 (mixed) | no |
| Epsilon GC instead of Serial | 19.96 vs 20.03, 22.45 vs 22.39 | no (no change, and a leak would be fatal) |
| compiler threads at a lower priority | 19.88, 23.32 (mixed, worse p99 on 7.0) | no |
| a 60 s warm-up before measuring | 20.23 vs 20.03, 22.86 vs 22.39 | (measurement only: JIT warm-up is not what these numbers measure) |
| GraalVM CE 25 native image (`native-image -O3`, FFM downcalls registered, handles built at image build time) | 12.04 vs 11.32, 20.01 vs 18.65 (6–7 % worse); RSS 83 vs 142 MB; the build peaks at 1.3–1.45 GB | no |

Oracle GraalVM 25.0.4 (which has native-image PGO) fails to build the image: its builder rejects the build-time downcall handles ("unexpected input could not be handled: linkToNative"), and with run-time handles a native image interprets every downcall (about 40 times slower). Moving the whole feed plan behind one C call was not tried, because the profile shows the boundary is not the cost.

Test-only knobs, never set on the box: `FEED_SLOTS` (a ring's table size; 2,000-slot tables served 10,000 users across six rings with no errors), `FEED_WRITE_US`, `FEED_CK_FRAMES`, `FEED_CK_HIGH`, `FEED_GATE_US`, `FEED_WAIT_US`, `FEED_PLAIN_LOG` (plain unix-excl, without the VFS), `FEED_LOOP_SYNC` (the loop syncs the log header as SQLite would), `FEED_WRITEBACK_KB`, `FEED_CACHE_PAGES`, `FEED_STATS` (a line a second), `FEED_JVM_FLAGS` (extra JVM flags), `-Dfeed.scalar=true` (no vector loop).

## Credit

The design is rust-iouring-kensac's (#264, MIT): direct descriptors in several rings, single-shot accepts over `SO_REUSEPORT` listeners, multishot receives into provided buffers, the buffer-ring retry for Ubuntu's kernels, one transaction per pass with responses held until COMMIT, B-tree cursor reads with a SQL fallback and a schema check, the checkpoint thread with writes held while the log's tail is copied, and its 4,096-frame checkpoint cycle. From goose-iouring-ahmadsh2007 (#291, MIT): the receive-buffer count, letting writes wait a few milliseconds to share a commit, and a VFS that writes a commit's frames at once. This entry is a port of goose-iouring-davidhsv (#384): `net.c` and `db.c` are its C files adapted to pointer-and-length calls, and `src/` follows `server.goose` function by function. Locking the code in memory, the hardening-off flags and PGO on a synthetic database come from c-iouring-pavlyhalim (#245) and mojo-iouring-oh-meh (#257); reading through SQLite's B-tree cursors from lean-derive-hargup (#153).

## Files

| | |
|---|---|
| `src/*.java` | the server |
| `net.c` | the kernel: rings, queued operations, completions, receive buffers, per-connection byte chunks, listeners |
| `db.c` | SQLite: the amalgamation, cursor moves and record addresses, the pass's transaction, the INSERTs, the checkpoint thread, the log VFS, disk timings |
| `feed.h` | what `libfeed.so` exports |
| `train/train.py` | profile training (build time only) |
| `install.sh`, `build.sh`, `start.sh` | |
