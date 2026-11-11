# Goose: an io_uring server written in Goose, SQLite B-tree reads

One thread serves every connection through io_uring and every request through one SQLite
transaction per pass. The server is [Goose](https://github.com/aardappel/goose), a memory-safe
systems language with no heap, no GC and no lifetime annotations, which compiles to C: the
event loop, the connection table, the accept and ring policy, batching and group commit, what
is answered now and what waits, the checkpoint policy, HTTP, JSON, HS256 and the read plans are
all `app.goose`. C is only what Goose cannot do: talk to the kernel (`uring.c`) and be SQLite
(`sqlite.c`).

| | |
|---|---|
| Language | Goose, compiler pinned at commit `369c7a2c` (runtime `3025e5810379`), built from source by `install.sh` (sha256-checked) |
| Framework | none: the event loop, HTTP/1.1, JSON and HS256 (SHA-256 and HMAC included) are written in Goose here |
| io_uring | liburing 2.15, pinned tarball (sha256), static |
| SQLite | 3.54.1 amalgamation (sha256), unmodified, compiled into `sqlite.c` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, profile-guided, static |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors); tested on Ubuntu 24.04's 6.8 |

## History on the box

| Commit | Users held | What changed |
|---|---:|---|
| `204b6b0` | 260,000 | first version; its C was a port of rust-iouring-kensac's transport and helpers |
| `67ef830` | 270,000 | the event loop moved into Goose (new `uring.c`, `sqlite.c`), coalesced writes, a new checkpoint policy |
| `74d44c9` | 270,000 | 16 MB of receive buffers per ring, one write per commit, 16,384-frame checkpoint cycles |
| this one | (not run yet) | rings made at startup, a limit on what one pass answers, 4 ms passes, more accepts in flight, shorter checkpoint cycles |

Every scored version failed just above its score the same way: p99 in seconds with 0 errors and
CPU to spare (`74d44c9`: 280,000 at p99 2.0 s, the 270,000 hold at p95 261 / p99 906 ms and
79 % CPU). Other entries hold 300,000 at 92-97 % CPU, so the box had room: something stalled,
and the server then took too long to get back. This version is about that. What changed, and
what each change measured here (the rig and its limits are described under Local measurements):

- **Nothing is allocated in the middle of a ramp.** The earlier versions added a ring whenever
  the last one filled, so the fifth ring (a 1 MB completion queue the kernel must find as one
  contiguous block, a 65,535-entry descriptor table) was made at 260,000 connections, at the end
  of a ramp on a 2 GB machine full of socket memory: exactly where all three versions fell
  over. The six rings that 384,000 connections need are now made at startup, and new connections
  go to whichever rings hold the fewest (41,487-41,746 per ring at 250,000 connections; the old
  fill-one-then-the-next left one ring full and the last nearly empty).
- **A pass answers at most 1,024 connections; the rest are read and wait in user memory.**
  Before, a pass answered everything it found, however much: after a stall that was one pass of
  seconds during which nothing was read or sent, each unread request pinned a 4 KB page of
  socket memory, TCP's memory limit was passed (186,629 pages against a limit of 45,180 on a 2 GB
  machine), the kernel dropped requests and clients retransmitted on timers. In a 250,000-user
  run with the server frozen once for about 5 s, the merged build never got back and was killed
  by the OOM killer at 1,402 MB; this one answered the backlog within the next 5-10 s, at a peak
  of 1,019 MB, with no errors. Short passes cost nothing at saturation (65,300 requests/s with
  this limit, 64,500-66,400 with 2,048 to 8,192, 65,200-65,400 for the merged build).
- **In order, except after a stall.** Waiting requests are answered oldest first, so an overload
  delays every request a little instead of some a lot (at saturation: p99 622-688 ms and no
  request over 826 ms here, where the merged build had a p99 of 770-786 ms and requests of
  3.1-3.3 s). Only when the loop sees that it did not run for over a second does it answer
  what arrives first, for a while: those requests are late whatever happens, and answering in
  order would make every request of the next seconds late behind them. (Answering what arrives
  first all the time was tried and is worse: under a sustained overload it strands a few
  percent of the requests for seconds.)
- **4 ms passes instead of 1 ms passes plus a 2 ms wait for writes.** A pass now waits for 512
  completions on ring 0 or 4 ms, and every request of those milliseconds, reads and writes,
  shares its transaction, its COMMIT, its system calls and its burst of sends. The separate
  hold-and-release of writes is gone (a write is held only while a checkpoint finishes). At
  300,000 users the server's own CPU per request went from 7.5 µs to 5.8 µs and the whole
  machine's busy time from 45.2 % to 41.9 %; at 150,000 users, from 7.7 to 6.4 µs and from 25.0 %
  to 21.3 %. The median response time goes up by a millisecond or two, which the challenge does
  not score.
- **About 2,000 accepts in flight instead of 512.** An accept is armed again only after a pass
  has seen it complete, so the number in flight bounds the connections accepted per pass. With
  passes of 4 ms that is plenty; with longer passes under load and a ramp of 5,000 connections
  a second it was not, and new connections sat in the listen queue with their first request
  for seconds.
- **Shorter checkpoint cycles** (8,192 frames, as in `67ef830`, with waits of at most 250 ms).
  What the checkpointer copies while writes wait is what was written during its first copy, so
  the wait grows with the cycle; `74d44c9`'s 16,384 frames gave the worst hold of the three on
  the box.
- **8 MB of receive buffers per ring instead of 16 MB**, and receives that end because a ring
  ran dry are armed again a bounded number per pass, which is what stops the re-arm storm that
  `74d44c9` answered with more buffers.

## What is Goose and what is C

| `app.goose` (Goose) | `uring.c`, `sqlite.c` (C) |
|---|---|
| the loop: waiting, draining every ring's completions, flushing after COMMIT, re-arming receives | `io_uring_enter`, queueing operations, copying completions into a Goose array |
| the connection table: generations, flags, which box holds what, partial, held and waiting requests | numbered byte boxes (C memory, since the kernel reads them asynchronously) |
| accepting: how many in flight, on which rings, when a ring is full, when to open another | rings, sparse direct-descriptor tables, provided buffers, listening sockets |
| how many connections a pass answers, which wait, in what order; the checkpoint policy, when writes wait | the checkpoint thread (it copies when asked) |
| HTTP/1.1 framing and parsing, routing, every status code and error message | |
| HS256: base64url, SHA-256, HMAC (precomputed pad states), the claims | |
| a validating JSON reader (any document, escapes, surrogate pairs, UTF-8) and writer (8-byte SWAR escaping) | |
| the body's JavaScript `trim()` and code-point count | |
| SQLite's record format: varints, serial types, column ranges | moving SQLite's B-tree cursors, copying a row's record bytes |
| the feed and post read plans and their fallback to SQL | BEGIN/COMMIT, the two INSERTs, `SELECT 1` |

Goose cannot hold a pointer into memory it does not own, so the boundary is copies, and no
Goose memory is ever shared with the kernel: C appends a message's bytes and a row's record
bytes to Goose builders, and Goose's finished responses are copied into a box for sending.

## A pass

1. The sends queued by the last pass are handed to the kernel on every ring. Ring 0 then waits
   for 512 completions or 4 ms (not at all while requests or receives are still queued), and
   only after the wait do the other rings collect their completions, so what arrived on them
   meanwhile is served by this pass like ring 0's.
2. Connections whose requests were read by an earlier pass and still wait are answered first,
   oldest first. Then each ring's completions are copied into a Goose array and handled in
   order. A receive is served at once while the pass has answers left: Goose parses every
   complete request in the message (after any bytes kept from an earlier one), answers it into
   the connection's output box, and keeps the rest. Past 1,024 answers the bytes are only moved
   out of the kernel's buffer into a small box and the connection joins the waiting list.
3. Every request that touches the database joins the pass's transaction, begun by the first one.
4. `COMMIT`, then every touched connection's output goes to the kernel: **no response from a
   pass is sent before its COMMIT returns** (group commit, rule 6). If the commit fails, every
   answer of the pass becomes a 500 and its connection closes, so a 201 never claims a lost
   write.
5. Receives that ended are armed again (at most 4,096 per ring), accepts are topped up on the
   rings with the fewest connections, and the checkpoint policy runs.

## Connections

- Accepted sockets are io_uring **direct descriptors** in a ring's file table, so
  `LimitNOFILE=65535` caps a ring, not the server. Six rings are made at startup (enough for
  384,000 connections at that limit; fewer rings when the limit is higher), and more are added
  only past that, up to 16. Accepts are single-shot, about 2,000 in flight, spread over eight
  `SO_REUSEPORT` listeners (eight accept queues) and over the rings that hold the fewest
  connections. A ring stops accepting while fewer slots than its accepts (plus 64) are free.
- One **multishot receive** per connection into the ring's provided buffers (8,192 x 1 KB):
  an idle connection holds no buffer, only its 24-byte record in Goose's table.
- One send in flight per connection; answers to pipelined requests queue behind it, in order.
  Closing is a `SHUTDOWN` hard-linked to a direct `CLOSE`; a generation number in every
  `user_data` drops completions for a reused slot. Idle connections are never closed.
- Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration with zero reserved words
  (Launchpad #2162843); a refused registration is retried once with `resv[0] = 1`.
- The binary's code (static, ~2 MB) is `mlock`ed at startup: with no swap, reclaim near the
  memory wall takes file-backed pages first, and the loop must not fault its own code back in.

## Writes: group-committed per pass

Every write of a pass's 4 ms shares its transaction and its COMMIT with the pass's reads: at
300,000 users that is about ten writes per commit, and the pages several of them touch (the
tail of `posts`, of the likes table, of the indexes on hot posts) go to the log once instead
of once each. Each response waits for the commit of its own write (rule 6). A write waits
longer only during the last step of a checkpoint (below): then that request, and anything
pipelined behind it on its connection, stays with the connection until writes are let through.

## SQLite

- **Reads walk SQLite's own B-tree cursors** inside the pass's transaction (only the bytecode
  engine is skipped; pages come through SQLite's pager, WAL and page cache), as the organizer
  allowed on 2026-10-31. `sqlite.c` moves a cursor and copies the row's record bytes into
  Goose; Goose decodes them and decides what to read next.
  - **Feed:** walk `posts_created_at_id_idx` from its first entry (it is `created_at DESC, id
    DESC`, so this is exactly the reference `ORDER BY`), then the 20 posts by rowid in
    ascending order (consecutive ids are the next cell of the same leaf), the 20 authors in
    `users`, and the like counts: one range of `likes_post_id_idx` counted in place on its leaf
    pages when the ids are within 512 of each other, a seek per post otherwise.
  - **Post:** a rowid seek on `posts`, one on `users`, and a like count.
  - **Nothing about the data is assumed**: not dense ids, not timestamps in id order. A post
    whose user row is missing (which the JOIN would drop) sends the feed to the reference SQL
    query. The column layout of every table and index decoded is checked at startup.
- **Writes are SQL**: `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`
  (a new like is told from a repeat by `sqlite3_changes`); the like's 404 check is a seek.
- WAL, `synchronous=NORMAL`, `unix-excl` (the WAL index in heap memory, no per-transaction
  file locks), 32 MB page cache, `mmap_size=0`, `wal_autocheckpoint=0`.
- **One write per commit**: the connections open through "goose", a VFS over unix-excl (SQLite's
  public VFS API). The database file is opened straight through. For the log, writes that
  continue exactly where the previous one ended (a transaction's frames, the header after a
  restart) are gathered, and written the moment the commit frame's page arrives: inside
  SQLite's own write call, so before the frames enter the WAL index and before COMMIT returns,
  when unix-excl would have finished writing them anyway. Any other write (a checksum rewrite),
  and every read, sync, truncate, size query, file control and close, first writes out what is
  gathered. A run goes out in pieces under the unix VFS's 128 KB limit per call.

## Checkpoints

A thread on its own connection, at nice 19 (so it runs in the loop's idle time), runs a PASSIVE
checkpoint whenever `app.goose` asks; the policy is Goose's:

- Past 8,192 log frames, ask for one. When it finishes, hold writes (reads go on) and ask for
  another, which copies the frames written meanwhile; when that one has copied the whole log,
  the next write starts the log over. A hold lasts at most 250 ms.
- If the log passes 32,768 frames while a checkpoint runs (the loop leaving the thread no idle
  time), writes are held too, and the loop gives the CPU away for 5 ms of every 50 until the
  log is back under, so an overload cannot grow the log without bound.
- The loop never runs a checkpoint itself.

## HTTP, JSON, JWT

- Only the headers the API uses are read. Requests split across receives are completed from
  the connection's box; pipelined requests are answered in order. Heads over 16 KB, bodies over
  1 MB, chunked bodies and malformed request lines get `400 bad request` and the connection
  closes (out of scope in SPEC). `Connection: close` and HTTP/1.0 close after the response.
- HS256 only; every token's signature is checked on every request (constant-time compare);
  only the key's padded blocks are hashed once at startup (the key schedule, not a result).
  Only the canonical base64url spelling of a signature is accepted. `exp` is required and must
  be in the future, then `sub` must be a positive integer string and `username` a string.
- The JSON reader validates whole documents (any nesting, with an explicit stack so no input
  can recurse), decodes escaped member names, and the last duplicate key wins, as in
  `JSON.parse`. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code
  points (the unit of the schema's `CHECK (length(body) …)`).

## Build

`install.sh` (root, once): `build-essential cmake ninja-build curl unzip python3`, then the
Goose compiler from its pinned source. It is one large C++20 translation unit; built with one
job at `-O2` and a tighter gcc collector it peaks at 1.34 GB (measured under a 1.6 GB cgroup
cap). It fit on the box for the scored runs. `install.sh` is unchanged from the merged version.

`build.sh` (user): fetches and checks SQLite and liburing, compiles `app.goose` to C
(`goose -O2 --standalone`), and builds `bin/server` static at `-O3` (`-march=x86-64-v3
-mtune=cascadelake`, 256-bit vectors, Ubuntu's default hardening off). **Profile-guided**: an
instrumented build serves `train/train.py`, which makes a synthetic database of its own (5,000
users, 40,000 posts, 160,000 likes, never the seed), signs its own tokens and drives the load
test's request mix plus the test suite's error paths over 32 connections. `PGO=0 bash build.sh`
skips it. The SIGTERM-to-`exit()` hook that writes the profile (`train/pgo_exit.c`) is linked
into the instrumented build only.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process; schema untouched; reads through SQLite's B-tree
cursors and pager, writes in SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: every request reads its rows from SQLite while it is served.
Kept between requests: prepared statements, cursor objects, `KeyInfo`, the four root page
numbers (schema metadata read at open) and the HMAC key schedule. Scratch buffers and boxes are
reused as storage only. A request that waits for a later pass waits as the bytes the client
sent; nothing is read from the database for it until it is answered.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for the commit of its pass.
7. Nothing detects the load test; the limit on a pass, the order of answers and the stall rule
apply to every request, whatever its route.
8–9. Direct on `HOST:PORT`.
10. Built from source on the box; Goose, SQLite and liburing pinned by commit/version and sha256.
11. Nothing on the box is tuned; the process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local measurements (not a score)

Not the organizer's box, and less like it than I would want. The machine is an Apple M4; the
"box" is a KVM guest (1 vCPU, 1,955 MB, no swap, Ubuntu 24.04, kernel 6.8.0-147, virtio disk
and network) nested inside a Linux VM on it, the server started by systemd as an unprivileged
user with `LimitNOFILE=65535` on port 80. The load is a native replay of `bench/load.js`'s user
model in the outer VM (one keep-alive connection per user, the same request mix and think
times at their real length, twelve source addresses), over a bridge. The merged build
(`74d44c9`) and this one were built the same way and run one after the other, each on a fresh
copy of the seed.

What the rig does not show: it is arm64, and nested virtualization makes the guest's kernel
networking far more expensive than on a real host (softirq is more than half of the guest's
busy time, and each new connection costs it about 80 µs). So the guest cannot take the load
test's 60 s ramp at these sizes with either build: at 5,000 new connections a second its
kernel alone uses most of the CPU, and near 240,000 connections it stops scheduling the server
at all. The runs at 250,000 and 300,000 users therefore ramp over 200-240 s, and the numbers
that mean something are the differences between two builds on the same rig, not the levels.
The guest's network interface polls on a 200 µs timer instead of taking an interrupt per burst
(a compensation for nested virtualization, where an interrupt costs many times a real one);
nothing else in the guest is tuned.

| | merged `74d44c9` | this version |
|---|---:|---:|
| **300,000 users**, 240 s ramp, 120 s hold: requests/s in the hold | 31,754 | 31,747 |
| p95 / p99 of the whole run | 4.2 / 7.2 ms | 6.8 / 10.0 ms |
| server CPU in the hold (user + system) | 23.9 % (8.2 + 15.7) | 18.5 % (8.4 + 10.1) |
| the guest's CPU busy in the hold | 45.2 % | 41.9 % |
| peak memory of the service's cgroup | 737 MB | 645 MB |
| **150,000 users**, 60 s ramp, 60 s hold, two runs each: server CPU | 12.2 %, 17.9 % | 10.2 %, 11.1 % |
| the guest's CPU busy | 25.0 %, 33.3 % | 21.3 %, 24.9 % |
| p99 in the hold | 4.5 ms, 279 ms | 5.8 ms, 6.9 ms |
| **250,000 users, the server frozen once** (SIGSTOP for about 5 s) | never recovers; OOM-killed at 1,402 MB, 1.27 million failed requests | normal again in the second 5 s window after it; 0 errors; peak 1,019 MB |
| requests of the 80 s hold that took over 1 s | (killed) | 7.1 % (5.5 s of requests: the freeze) |
| **saturation**, loopback in the outer VM, 60,000 users at 20x speed, two runs: requests/s | 65,189, 65,397 | 65,279, 65,293 |
| p99 / slowest request | 770-786 ms / 3.1-3.3 s | 622-688 ms / 768-826 ms |

Correctness, on the build as pushed:

- `test/run.sh`: 42/42; also 42/42 with one answer per pass (every request takes the waiting
  list), with a checkpoint cycle every 256 frames (writes are held all the time) and 256-slot
  descriptor tables, and with 1 ms passes.
- The same 50 raw-request cases (pipelining up to 300 requests, requests split byte by byte,
  bodies split across writes, every token and body failure, `Connection: close`, HTTP/1.0,
  malformed and chunked requests) against the merged build and this one, in the default
  configuration and with one answer per pass and held writes: byte-identical status and body
  (row timestamps masked).
- kill -9 during writes, 8 rounds (16 writers, killed at a random moment; half the rounds with
  512-frame checkpoint cycles and 4 answers per pass): every acknowledged post and like (33,893
  and 33,393) was in the database afterwards and `integrity_check` said ok.
- 5,000 users at 20x speed for 30 s with 512-slot tables (12 rings, 6 of them added on the
  way), 256-frame cycles and 8 answers per pass: 307,894 requests, 0 errors.

Test-only knobs, never set on the box: `GOOSE_SLOTS`, `GOOSE_WAIT_US`, `GOOSE_ANSWERS`,
`GOOSE_CK_FRAMES`, `GOOSE_PLAIN_VFS` (plain unix-excl) and `GOOSE_STATS` (a line every second:
passes, the longest pass split into serving and commit, completions per pass, buffer
exhaustion and re-arms, requests deferred and waiting, boxes in use, connections per ring, the
log, holds, restarts and stalls seen).

## Credit

Ideas from earlier entries, all MIT, with the code written for this entry: direct descriptors
in several rings, single-shot accepts kept in flight over `SO_REUSEPORT` listeners, multishot
receives into provided buffers, the buffer-ring retry for Ubuntu's kernels, holding writes
while a checkpoint copies the log's tail, and reading everything first while answering a
bounded number per pass ("drain first") come from rust-iouring-kensac (#264); making every
ring at startup from rust-iouring-rkkautsar (#355); counting likes on the index's leaf pages,
locking the code in memory, hardening-off flags and PGO on a synthetic database from
c-iouring-pavlyhalim (#245) and mojo-iouring-oh-meh (#257), whose README also describes waiting
a little so that batches form; reading through SQLite's B-tree cursors from lean-derive-hargup
(#153), as the organizer ruled.

## Files

| | |
|---|---|
| `app.goose` | the server: the loop and connection table, accepts and rings, what waits and in what order, checkpoint policy, HTTP, routing, JWT/SHA-256/HMAC, JSON, record decoding, read plans |
| `uring.c` | the kernel: rings, queued operations, completions, receive buffers, byte boxes, listeners |
| `sqlite.c` | SQLite: the amalgamation, cursor moves and row copies, the pass's transaction, writes, the checkpoint thread |
| `train/train.py`, `train/pgo_exit.c` | PGO training (build time only) |
| `install.sh`, `build.sh`, `start.sh` | |
