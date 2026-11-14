# C, io_uring, SQLite's B-tree cursors, one thread

One event-loop thread serves every connection through io_uring and every request of a batch
through one SQLite transaction. Reads walk SQLite's own B-tree cursors inside that transaction and
are written straight from the pages into the response; writes are prepared SQL with group commit.
Nothing on the request path allocates, and nothing on the loop ever fsyncs: two more threads only
run WAL checkpoints.

| | |
|---|---|
| Language | C11, gcc 13 (Ubuntu 24.04 `build-essential`), one translation unit with the SQLite amalgamation |
| Framework | none: own io_uring layer (raw system calls, no liburing), HTTP/1.1 framing, JSON, HS256 |
| Database | SQLite 3.54.1 amalgamation, unmodified, SHA-256 pinned, downloaded by `build.sh`; `unix-excl` VFS under a small write-coalescing shim |
| Build | `-O3 -march=x86-64-v3 -mtune=cascadelake`, static, profile-guided (trained at build time on a synthetic database), 256-bit vectors only |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot accept and receive, provided-buffer rings, direct descriptors); tested on 6.8 and 6.18 |

## Connections

Every k6 user holds a keep-alive connection, so the server is sized for hundreds of thousands of
mostly idle sockets under `LimitNOFILE=65535`.

- Accepted sockets are io_uring **direct descriptors**, in each ring's fixed-file table rather than
  the process's descriptor table. A table can hold at most `RLIMIT_NOFILE` entries, so the server
  uses up to 10 rings of 65,471 slots. The rings 330,000 connections need (`PRE_CONNS`) are made
  at startup, while the box is idle: a ring's completion queue is ~1 MB of physically contiguous
  kernel memory, and asking for it mid-ramp, with a gigabyte of socket slab fragmenting the box's
  memory, can stall the loop in page compaction. Any further ring is made when connections need
  it (8,192 before the one before it is full). The accepting ring moves on 6,144 slots before its
  table is full: an accept that finds no free slot has already taken the connection off the
  listen queue, and the kernel would drop it.
- Eight listening sockets share the port (`SO_REUSEPORT`), each with its own accept queue of
  `somaxconn`, so together they hold ~32k pending connections through a ramp or a stall. Their
  multishot accepts are armed on the ring being filled and cancelled by user data when it moves on.
- Each connection has one **multishot receive** into the ring's provided-buffer ring (1,024 × 2 KB),
  so an idle connection holds no receive buffer and no heap, only a 32-byte record, and a request
  costs one completion. Requests are parsed in place in the kernel's buffer and the buffer goes
  back at once.
- One `SEND` per response, headers and body in one buffer (`MSG_WAITALL`, so the completion
  means all or nothing). Output comes from per-batch arenas that go back to a pool once every
  send that points into them has completed, so idle connections hold no output memory either.
- Every ring is `SINGLE_ISSUER | DEFER_TASKRUN`: completions are posted only when the loop enters
  the ring, so receive copies happen in batches on the loop's own thread. Each pass first gives
  every other ring one non-blocking enter (submits its sends, collects its completions), then
  ring 0 waits for 64 completions or 2 ms (`WAIT_MIN`, `WAIT_US`), whichever comes first, so
  bigger batches share one commit and a pass never spins; while a commit group is open the wait
  ends at the group's deadline, and a batch of fewer than 32 completions (`CG_MIN`) commits at
  once, so a lone request is answered within its batch (the heads before `2703df3` let it wait
  for the idle timeout, which starved the PGO training client on the box).
- **Every ring is entered twice per batch** (`SWEEP2`). With deferred task work
  (`IORING_SETUP_DEFER_TASKRUN`) a ring's completions are posted only inside an enter on that
  ring, and the one non-blocking enter per batch on the rings the loop does not wait on does not
  always post them. Found with a load generator holding 300,000 connections (one per user) and
  batches stretched to 3 ms, which is what the box's busier core does at 1 ms: ~6,000 server
  sockets with an unread request, nothing queued in user space, p99 1.4 s with 0 errors, and,
  with each response tagged by the ring that served it, every stuck request was on one of the
  rings the loop did not wait on, one ring at a time for a second or more, never on ring 0. That
  is the box's failure at 290,000-300,000 (`1e7fb4d`, `f4ec9c5`: p99 1.3-3.8 s, 0 errors, 78-84 %
  CPU). Entering every ring again after the batch's database work, before the commit, posts
  what the first enter left behind: the same run passes with p99 12 ms and no ring ever holds a
  stuck request. A ring that rotates the wait instead also passes (p99 37 ms) but less well. The
  second sweep costs 5 % of loop CPU at a 1 ms window; the 2 ms window it makes safe again is
  10 % cheaper, so together they are 5 % under the 1 ms head (41.9 s against 44.2 s of loop CPU
  for 3.0 M requests). Commit groups of 20 ms saved another 2 % of the cgroup's CPU but doubled
  the softirq time on the server's core (the replies of a group leave as one burst), which on
  the box is the same core, so groups stay at 3 ms. Cooperative task work instead of deferred
  has no stuck rings either but costs 13-20 % more CPU.
- **After a burst, reads are bounded.** When a stall ends every connection has a request waiting
  and the multishot receives would use up the buffer ring at once; the rest end with `ENOBUFS`.
  Such a connection is not re-armed but queued, and each pass issues one `MSG_DONTWAIT` read per
  queued connection, never more on a ring than it has buffers, in rounds that complete inside the
  submit. A read that fills its buffer is queued again; a shorter one, or `EAGAIN`, means the
  socket is drained and the multishot receive is armed again.
- **Drain first.** A pass reads up to 16,384 times but answers at most 2,048 requests; the rest
  wait in the connection's own buffer (a copy the size of the request) on a ready list that later
  passes answer first, oldest first. A request left in its socket costs a 3–4 KB kernel buffer,
  and a backlog of those is what runs a 2 GB box out of memory.
- Closing is a `SHUTDOWN` hard-linked to a direct-descriptor `CLOSE`; a generation number in every
  `user_data` drops completions for a reused slot. Idle connections are never closed by the server.
- The binary's code and data are locked in RAM (`mlock`, best effort), the connection tables and
  receive buffers are faulted in when a ring is made, and the page-cache arena at startup, so
  nothing is left to be found when memory is shortest.

## A batch

1. Every completion is handled. Requests are answered straight into the arena, up to 2,048 a
   batch; a connection gets at most one request per batch (pipelined bytes wait for the send).
2. All of the batch's reads and writes run in one SQLite transaction, begun lazily by the first of
   them (`BEGIN`, then the read transaction and four B-tree cursors). Reads see the batch's earlier
   writes.
3. `COMMIT`, then every response is handed to the kernel. **No response from a batch is handed to
   the kernel until its COMMIT returns.** A transaction that wrote stays open for up to 3 ms
   (`CG_US`), so the batches of those milliseconds share one commit: every commit rewrites the
   same hot pages into the log (page 1, the tails of posts, likes and their indexes), and sharing
   a commit across three or four batches cuts that traffic, and the checkpoint and writeback work
   behind it, by the same factor. The responses of the batches in a group wait for the group's
   commit, so a POST or a read in a group answers up to 3 ms later than it would alone; a
   transaction that has not written commits at once (nothing to group). If the commit fails, every response that touched the
   database becomes a 500 and the connection is closed, so no 201 ever claims a write that did not
   land and no read ever reports a row that rolled back. If SQLite ends the transaction by itself
   mid-batch (`SQLITE_FULL`, an I/O error), the rest of the batch does not write and its commit
   reports the failure.

## SQLite

- `GET /feed` and `GET /posts/:id` run on SQLite's B-tree cursors (`sqlite3BtreeFirst/Next/Previous`,
  `sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`), inside the batch's transaction, through
  SQLite's pager, WAL and page cache; what is skipped is the bytecode interpreter, since both
  queries are known ahead of time. The organiser allowed this reading of rules 2/3 on 2026-10-31.
  - **Feed:** the 20 newest `(created_at, id)` pairs are the first entries of
    `posts_created_at_id_idx` (a `DESC, DESC` index), so the index order is the feed order and
    nothing is assumed about the data. The like counts of all 20 come from one walk of
    `likes_post_id_idx` when the ids lie within 512 of each other: the index is ordered by
    `post_id`, so the newest posts' likes are its last entries, and when the last entry is within
    64 ids of the feed the walk runs backwards from `sqlite3BtreeLast` (free while the cursor still
    sits there), otherwise forwards from a seek. The cells are decoded in place a page at a time;
    the cursor's position is only read, and a used-up page is left by a seek. A seek per post
    otherwise. Then the rows are read in feed order (an id one below the previous one is one
    cursor step back; their pages get an extra reference while the other cursors move), the 20
    authors are looked up in one descent of `users` through the same pager calls as
    `sqlite3BtreeTableMoveto` (every key's child page chosen before the next level is fetched, a
    page shared by several keys fetched once, and inside a page the search starting where a
    straight line through its first and last rowids puts the key, since rowids are dense: 2-3
    probes instead of 7-9; the idea is from c-iouring-pavlyhalim #245), and every post object is
    written from the pages: no row is copied anywhere else.
  - **Post:** the row by rowid, its author, its likes, the same way. Likes check that the post
    exists with a seek too.
  - **Nothing is assumed and nothing is kept.** The stored layout is checked against the schema
    the decoders were written for at startup, and anything unexpected during a read (a record on
    overflow pages, a cell not in the expected form, an index entry without its row, a post whose
    author row is gone) makes that request run the reference SQL instead. Only prepared statements,
    cursors, KeyInfo objects and the four root page numbers (schema metadata read at open) are
    reused between requests; every request reads its rows from SQLite while it is served.
- Writes are prepared SQL: `INSERT … RETURNING id, created_at` for posts (`created_at` is the
  column default, sent as stored) and `INSERT … ON CONFLICT (user_id, post_id) DO NOTHING` for
  likes, `sqlite3_changes` telling 201 from 200.
- `unix-excl` VFS: this process is the only one that opens the file, so the wal-index lives in heap
  memory shared by the three connections and no `fcntl()` lock is taken per transaction.
- A small VFS shim over `unix-excl` (SQLite's public VFS API, the pattern of its own `vfstrace`)
  writes each commit's WAL frames with one `pwrite`: SQLite writes a transaction's frames one after
  another, header then page, and the commit frame's header carries the database size, so the shim
  buffers writes that continue exactly where the previous one ended and writes the buffer when the
  commit frame's page arrives, inside SQLite's own call, before the wal-index is updated and before
  `COMMIT` returns. A write that does not continue the run (the first after open, the first after
  the tail thread restarted the log) flushes the run and starts a new one; it never bypasses the
  buffer (the earlier heads let it, and a frame header written that way lost its commit flag, so
  the page behind it waited in user space after `COMMIT` had returned; a checkpoint reading that
  frame in that moment copied garbage, seen once the tail thread began restarting the log). A
  write that continues the run but does not fit the 128 KB buffer (a commit of 32 frames or more:
  any write batch released after a hold of more than ~13 ms) flushes the run but keeps the commit
  flag, since the flag belongs to the header that just went out and the page behind it must still
  go out before `COMMIT` returns (`1e7fb4d` cleared it there, and under CPU contention a
  checkpoint copied such a page before the loop had written it: "database disk image is
  malformed" from then on, found with a load generator holding 300,000 connections while a
  busy process shared the core). Every other call on
  the file flushes first and goes straight through, so the bytes and their order are SQLite's.
  Readers find frames only through the wal-index, so none can see a buffered one. (After
  c-iouring-pavlyhalim #245 and c-iouring-shivanshthapliyal #250.)
- SQLite's default page cache sits behind its plugin interface (`SQLITE_CONFIG_PCACHE2`) with one
  decision changed: at commit, when no page past the end of the file can be in the cache, the
  truncate does not walk the whole hash table (pcache1 never lowers its largest-key record, and a
  page split briefly parks a page at the lock-byte page's number, after which every commit scanned
  every slot). The idea is from c-iouring-pavlyhalim #245. The cache's memory comes from one arena
  handed to SQLite with `SQLITE_CONFIG_PAGECACHE`, backed by transparent huge pages where the kernel
  offers them.
- WAL, `synchronous=NORMAL`, `wal_autocheckpoint=0`, 64 MB page cache (warmed at start with the feed pages and the likes primary-key index), no `mmap` (a page in
  SQLite's own cache keeps its parsed header and is found without a wal-index lookup).
- **Checkpoints converge in the background; nothing on the loop ever fsyncs** (after
  rust-iouring-kensac #264, changed for one vCPU). The policy is the leader's, as a whole: past
  4,096 WAL frames (16 MB) a thread at nice 19 runs one PASSIVE checkpoint on its own connection,
  in the loop's idle time; whatever that pass could not copy (the frames written meanwhile) the
  tail takes under a hold of at most 250 ms; a missed restart costs one short cycle. This entry
  tried its own policy first and the box never favoured it: 64 MB cycles with passes repeated
  until fewer than 64 frames remained, holds of 400 ms growing to 800 ms, scored 270,000-290,000
  across eleven heads, and changing one parameter at a time toward the leader's (a flat 250 ms
  hold, `5d2fa6b`, 270,000; the bulk thread at nice 12, `ca5743b`, 270,000) changed nothing the
  box could resolve. `CKPT_FRAMES`, `CKPT_TAIL` (converge to this few frames first),
  `HOLD_MAX_MS` and `CKPT_NICE` keep the old policy one environment variable away. Then
  writes are held (reads go on; the next POST on each connection waits in its buffer) while a
  second thread at the normal priority copies that remainder: a few milliseconds of copying plus
  the syncs of the log and of the database, on a thread. That thread then restarts the log itself
  with a one-frame transaction (`PRAGMA user_version`): SQLite syncs the log header when a log is
  reused (`synchronous=NORMAL`), and this way that sync, the last one the loop used to do, lands
  on the thread too. The loop's reads go on meanwhile from the database file (read mark 0), which
  a restart does not disturb; a read that began earlier holds another mark and makes the attempt
  `BUSY`, so it is retried, and if the hold ends first the loop's next write restarts the log as
  before. A hold
  starts and ends only between batches and lasts at most 250 ms (writes only, once per cycle):
  if the disk makes the tail's sync slow, writes resume and the next cycle waits for half a log
  more, so a slow disk never gives back-to-back holds, and the log is allowed to grow (a longer
  log costs the loop nothing while its page cache holds the hot pages: the wal-index is
  consulted only on a cache miss) until the disk gives a restart. Before each pass the bulk
  thread reads the log sequentially ahead (`readahead`): a pass reads frames in page order,
  random in the file, and once the box's memory pressure has evicted them (they are clean once
  written back) that would be a disk seek per frame. The same thread can keep the database
  file's first pages warm between passes (`DBWARM_MB`, `readahead` every 5 s, only while
  MemAvailable is over 512 MB); it is off by default, as the leader has no such thing.
  (`42abbf3`, 270,000 with p99 179 ms at 270k, had a 512-frame tail and a 100 ms limit: on the
  box the tail's sync of a few hundred random pages outlasted the limit, the log never restarted,
  and the last resort copied half a gigabyte on the loop once per probe, the one stall that
  failed every 280k probe at 78 % CPU.) While idle, the bulk thread starts the writeback of the log pages
  written since its last look every 250 ms (`sync_file_range`, no waiting), so the log's dirty
  pages reach the disk continuously rather than in a burst at the next fsync, and the tail's
  sync finds little left. Two earlier designs scored worse
  on the box. `c3fdb10` (270,000 users) copied the tail without the convergence: on one vCPU it
  had grown to tens of MB during the slow bulk, the thread competed with the loop for the core and
  the holds ran into their limit, so every request saw the cycle (p95 114 ms, p99 551 ms at 77 %
  CPU). `f2f9227` (253,000) copied the small tail on the loop itself: its two fsyncs stalled the
  loop for seconds on the droplet's disk (p99 3.5 s on a hold). Past 1,048,576 frames (4 GB, a
  disk that gave no restart for minutes) the loop checkpoints by itself, as a last resort.
- Build options: `THREADSAFE=2` (each connection is used by one thread), `DEFAULT_MEMSTATUS=0`,
  `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`,
  `USE_ALLOCA`, `TEMP_STORE=3`, `OMIT_` deprecated/progress-callback/shared-cache/load-extension/
  autoinit/UTF16/trace/complete/get-table/decltype/JSON, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

## HTTP, JSON, JWT

- One pass over the head finds the request line and the `Authorization`, `Content-Length`,
  `Connection` and `Transfer-Encoding` headers (case-insensitive). A request split across receives
  is kept on the connection until it completes. Heads over 16 KB, bodies over 1 MB and chunked
  bodies get 400 and the connection is closed (out of scope in SPEC).
- Responses are built body-first in the arena; the status line and headers are written right in
  front of the body, so there is no copy and no `Content-Length` placeholder. JSON strings are
  scanned for characters that need escaping and copied in the same pass, 32 bytes at a time (16
  for timestamps and names); integers are written in place from a two-digit table. Measured on
  the load test's own data (short posts by random users, a few likes each), a feed is about
  50,000 instructions, 46 % of them the author lookups.
- Request bodies are fully validated JSON (`JSON.parse` semantics: the last duplicate key wins,
  escaped keys are decoded). `body` is unescaped once, trimmed with JavaScript's `trim()` whitespace
  set and counted in code points, the unit of the schema's `CHECK (length(body) …)`.
- HS256: the HMAC key's inner and outer states are computed once from `JWT_SECRET` (the key
  schedule, not a verification result). Every token is verified on every request: signature first
  (constant-time compare, canonical base64url only), `alg` must be `HS256`, `exp` required and in
  the future; then `sub` must be a positive integer string and `username` a string.

## Build

`install.sh` installs `build-essential`, `curl`, `unzip`. `build.sh` downloads the pinned
amalgamation, builds an instrumented binary, generates a synthetic database (5,000 users, 40,000
posts, 160,000 likes of random words, no seed data) and drives the load test's request mix plus the
test suite's error paths through it with `src/train.c` over 48 keep-alive connections, then builds
the final binary with the profile. `PGO=0 bash build.sh` skips that. No 512-bit vectors: on Cascade
Lake one would lower the core's clock for a while after each use.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process, schema untouched. Reads use SQLite's own B-tree
cursors through SQLite's pager; writes use SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: no response, query-result or token caches. Every request reads SQLite
pages while it is served. Kept between batches: prepared statements, cursors, root page numbers and
column layout (schema metadata, read at open), and the HMAC key schedule.
6. WAL + `synchronous=NORMAL`; group commit (one commit per up to 3 ms of batches); each response waits for its commit.
7. Nothing detects the load test: every route works for any request, and the server never sheds
load, never closes a healthy connection and never looks at who is asking.
8–9. Direct on `0.0.0.0:80`. Direct descriptors need no file descriptors beyond the listeners.
10. Built from source on the box; SQLite 3.54.1 pinned by SHA-256; the PGO profile is produced
during the build, nothing is committed but source.
11. Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard limit.

Credits: the io_uring ring policy, the read queue and the checkpoint state machine follow
rust-iouring-kensac (#264); the WAL-write shim, the page-cache wrapper and the batched author
descent follow c-iouring-pavlyhalim (#245) and c-iouring-shivanshthapliyal (#250). All code here
is new.

## Layout

```
src/uring.h   raw io_uring ABI: rings, direct descriptors, buffer rings, SQE builders
src/server.c  the event loop, batches, arenas, connection tables; includes sqlite3.c
src/app.h     the five endpoints
src/db.h      SQLite: transactions, statements, the VFS shim, the page cache, checkpoints
src/btree.h   the read plans on SQLite's B-tree cursors
src/http.h, json.h, jwt.h, sha256.h
src/train.c, train.sh   PGO training client and driver (build time only)
```

## Local measurements (not a score)

WSL2 on a desktop CPU, loopback, server pinned to one core, k6 on the others, the load test's
user model with think times divided by 50 (4,000 users, ~21,000 requests/s, 60 s, 1.42 M requests),
the same fresh seed copy and harness for every entry, process CPU time (kernel included) divided
by requests:

| Entry | CPU per request | p95 | p99 | errors |
|---|---:|---:|---:|---:|
| this one (two runs) | 11.3 µs, 11.4 µs | 1.6 ms | 2.1 ms | 0 |
| rust-iouring-kensac #264 | 12.1 µs | 1.7 ms | 2.6 ms | 0 |
| c-iouring-pavlyhalim #245 | 14.7 µs | 2.6 ms | 3.0 ms | 0 |

Other local runs: 5,000 users over three rings of 1,936 slots (ring switching), 0 errors at
26,000 requests/s; 60,000 users with the real think times over three rings, 0 errors, 72 MB of
process memory; receive buffers starved to 16 per ring, 0 errors (the read queue absorbs it).
Loopback charges the client side's receive work to the server too, so these are only comparable
with each other.
