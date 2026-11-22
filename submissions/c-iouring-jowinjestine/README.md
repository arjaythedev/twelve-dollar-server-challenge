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
- Each connection has one **multishot receive** into the ring's provided-buffer ring (8,192 × 1 KB),
  so an idle connection holds no receive buffer and no heap, only a 32-byte record, and a request
  costs one completion. Requests are parsed in place in the kernel's buffer and the buffer goes
  back at once.
- One `SEND` per response, headers and body in one buffer (`MSG_WAITALL`, so the completion
  means all or nothing). Output comes from per-batch 1 MB arenas, each its own mapping, that go
  back to a pool of 16 once every send that points into them has completed; arenas a burst needed
  beyond that are unmapped. Idle connections hold no output memory either.
- Every ring is `SINGLE_ISSUER | DEFER_TASKRUN`: completions are posted only when the loop enters
  the ring, so receive copies happen in batches on the loop's own thread. Each pass first gives
  every other ring one non-blocking enter (submits its sends, collects its completions), then
  ring 0 waits for 64 completions or 2 ms, whichever comes first, so bigger batches share one
  commit and a pass never spins. Every pass enters every ring, so fewer, fuller passes are less
  CPU: measured at 300k connections, ~4 % less loop CPU than 256 completions / 1 ms, with p99
  unchanged. (4 ms batches were tried: they cut the WAL ~40 %, one set of hot-page frames per
  commit, but under a CPU quota they add 25-50 ms to p95 at high load.)
- **After a burst, reads are bounded.** When a stall ends every connection has a request waiting
  and the multishot receives would use up the buffer ring at once; the rest end with `ENOBUFS`.
  Such a connection is not re-armed but queued, and each pass issues one `MSG_DONTWAIT` read per
  queued connection, never more on a ring than it has buffers, in rounds that complete inside the
  submit. A read that fills its buffer is queued again; a shorter one, or `EAGAIN`, means the
  socket is drained and the multishot receive is armed again.
- **Drain first.** A pass reads up to 16,384 times and answers up to 16,384 requests; the rest
  wait in the connection's own buffer (a copy the size of the request) on a ready list that the
  next pass answers before it reaps anything new, oldest first. A request left in its socket
  costs a 3–4 KB kernel buffer, and a backlog of those is what runs a 2 GB box out of memory.
- **Reaped during a long pass.** Answering 16,384 waiting requests takes ~0.4 s, and nothing was
  reaped meanwhile: the receive buffers ran dry, new requests stayed in their sockets, and each
  socket holding one counts at least a page against `tcp_mem` (~116 MB to pressure on 2 GB). Past
  ~30k of them TCP went into memory pressure and pruned queues (CPU, drops, client retransmits)
  until the OOM killer ended the run. Now the pass reaps every 512 answers (`REAP_EVERY`); what
  arrived goes into the connections' buffers behind the requests already waiting, and the receive
  buffers go back. Measured at 285k users in a 1 vCPU / 2 GB guest: no `ENOBUFS`, TCP memory
  stayed under 31 MB, no OOM kill, longest loop turn 0.65 s (was 1.9 s).
- **A backlog leaves no memory behind.** A connection's buffer is freed as soon as nothing waits
  in it, and at the first stats tick after a backlog has cleared, `malloc_trim` hands the freed
  heap back to the kernel. (Before, every connection a backlog touched kept its buffer for good:
  at 300k users in a 2 GB guest the process grew from ~175 to 365-380 MB and the kernel's OOM
  killer took it; now it peaks at ~260 MB with a 30-40 MB heap. The stats line prints `heap`,
  `heap_free`, `arenas` and `trims`.)
- Closing is a `SHUTDOWN` hard-linked to a direct-descriptor `CLOSE`; a generation number in every
  `user_data` drops completions for a reused slot. Idle connections are never closed by the server.
- The binary's code and data are locked in RAM (`mlock`, best effort), the connection tables and
  receive buffers are faulted in when a ring is made, and the page-cache arena at startup, so
  nothing is left to be found when memory is shortest.

## A batch

1. Every completion is handled. Requests are answered straight into the arena: what arrived
   since the last batch first, as much of it as can be answered within the kernel's 40 ms, then,
   up to 8 ms of work in all, the requests earlier batches left waiting, oldest first (see
   "Newest first" at the end); a connection gets at most one request per batch (pipelined bytes
   wait for the send).
2. All of the batch's reads and writes run in one SQLite transaction, begun lazily by the first of
   them (`BEGIN`, then the read transaction and four B-tree cursors). Reads see the batch's earlier
   writes.
3. `COMMIT`, then every response is handed to the kernel. **No response from a batch is handed to
   the kernel until its COMMIT returns.** If the commit fails, every response that touched the
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
  frame in that moment copied garbage, seen once the tail thread began restarting the log). Every other call on
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
  rust-iouring-kensac #264, changed for one vCPU). Past 16,384 WAL frames (64 MB) a thread at
  nice 19 runs PASSIVE checkpoints on its own connection in the loop's idle time, after a
  read-ahead of the log (up to 256 MB: the pass reads frames in page order, random in the file).
  Under steady writes a pass cannot copy the frames written meanwhile, and the log only restarts
  once it is fully copied; so that thread runs pass after pass, each copying what the previous one
  missed, until fewer than 64 frames remain. Then writes are held (reads go on; the next POST on
  each connection waits in its buffer) while a second thread at the normal priority copies that
  remainder: a few milliseconds of copying plus the syncs, on a thread. **The loop's next write
  restarts the log** (as in kensac's entry), so the loop's page cache survives the cycle; a log
  restarted by another connection makes SQLite drop that connection's whole cache at its next
  read, and the hot pages would be read again every cycle (~105-119k cache misses per run against
  ~8.9k, measured with 2 ms added to every disk access). The loop does the one header sync of a
  restart, once per 64 MB of log. `TAIL_RESTART=1` gives the restart to the tail thread.
- **Only SQLite's cache holds the database.** Every 2 s (`DB_DROP_MS`) the bulk thread drops the
  kernel's cached pages of `$SQLITE_PATH` (`posix_fadvise(DONTNEED)`; dirty pages are only queued
  for writeback). The pages requests use are in SQLite's own 64 MB cache, warmed at start, which
  is anonymous memory and, with no swap, never evicted; the kernel's copy of the rest of the
  ~290 MB file is what pushes a 2 GB box into reclaim when 300k sockets already hold ~1.2 GB of
  kernel memory. (The organiser's 2026-11-15 ruling allows this for the entry's own files.)
- **The log stays bounded.** A nice-19 thread gets ~1.5 % of a saturated core, and nothing else
  stopped the log from growing (short of the 4 GB last resort below), each frame staying in the
  page cache at the box's memory wall. Past 32,768 frames (128 MB, `CKPT_URGENT`) the passes go to
  a third checkpoint thread at nice 10 until the log restarts: ~10 % of the core against the loop,
  enough to catch up (a checkpoint that keeps up needs ~1 %). (`297c8ea` had such a thread at the
  normal priority, and on the box it ran against the loop as an equal minutes into a hold. A
  thread of its own because an unprivileged thread can raise its nice value but never lower it
  again.)
- A hold starts and ends only between batches and lasts at most 400 ms (writes only, once per
  cycle: under 0.5 % of requests): if the disk makes the tail's sync slow, writes resume and the
  next cycle waits for half a log more, so a slow disk never gives back-to-back holds; for a log
  past 128 MB the limit grows to 800 ms and no further (a POST that waited a second would count
  against the score). `91da0e6` had the loop restart the log with 16 MB cycles (four times the
  header syncs on the loop) and no read-ahead, and scored 266,000 (it passed 280k for two minutes,
  then failed the five-minute hold with its memory peak at 1.6 GB); `8193037`, with the tail
  thread restarting the log and 64 MB cycles, held 280,000 at an 810 MB peak. (`42abbf3`, 270,000 with p99 179 ms at 270k, had a 512-frame tail and a 100 ms
  limit: on the box the tail's sync of a few hundred random pages outlasted the limit, the log
  never restarted, and the last resort copied half a gigabyte on the loop once per probe, the one
  stall that failed every 280k probe at 78 % CPU.) While idle, the bulk thread starts the
  writeback of the log pages written since its last look every 250 ms (`sync_file_range`, no
  waiting), so the log's dirty pages reach the disk continuously rather than in a burst at the
  next fsync, and the tail's sync finds little left. Two earlier designs scored worse on the box.
  `c3fdb10` (270,000 users) copied the tail without the convergence: on one vCPU it had grown to
  tens of MB during the slow bulk, the thread competed with the loop for the core and the holds
  ran into their limit, so every request saw the cycle (p95 114 ms, p99 551 ms at 77 % CPU).
  `f2f9227` (253,000) copied the small tail on the loop itself: its two fsyncs stalled the loop
  for seconds on the droplet's disk (p99 3.5 s on a hold). Past 1,048,576 frames (4 GB, a disk
  that gave no restart for minutes) the loop checkpoints by itself, as a last resort.
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
the final binary with the profile. The instrumented build counts with `-fprofile-update=prefer-atomic`:
the checkpoint thread runs SQLite during training too, and racing counters once left a profile the
optimized build rejected. `PGO=0 bash build.sh` skips that. No 512-bit vectors: on Cascade
Lake one would lower the core's clock for a while after each use.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process, schema untouched. Reads use SQLite's own B-tree
cursors through SQLite's pager; writes use SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: no response, query-result or token caches. Every request reads SQLite
pages while it is served. Kept between batches: prepared statements, cursors, root page numbers and
column layout (schema metadata, read at open), and the HMAC key schedule.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for its batch's commit.
7. Nothing detects the load test: every route works for any request, every request that is read
is answered, and the server never closes a healthy connection and never looks at who is asking.
The one limit is the box's memory: `CONN_LIMIT` connections at once; the ones over it wait in
`connect()` or are answered 503 (see the last section).
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

## Switches from #479 / #480 (all off but the SHA-256 schedule)

#479 and #480 tried five changes on top of `e487a70` (#441, 285,000 on the box). Their rig runs
held 230,000 and 270,000. Measured here one at a time against `e487a70` (local, server on one
core, 280,000 users, 60 s ramp + 90 s hold, two interleaved rounds, the same request rate in
every run):

| Build | App CPU | Hold p50 / p99 |
|---|---:|---:|
| `e487a70` | 33 %, 34 % | 4.5 / 7.0-7.5 ms |
| eventfd wake-ups + rolling SHA-256 | 40 %, 39 % | 2.2 / 4.3-4.5 ms |
| rolling SHA-256 only | 34 %, 33 % | 4.5 / 7.0-7.9 ms |

- **Rolling SHA-256 schedule** (on; `SHA256_ROLLING16=0` at build time restores the 64-word
  one): 16 words in a ring instead of 64, for the JWT HMAC of every write. CPU-neutral within the
  noise above (writes are ~8 % of requests), kept for the smaller stack. `build.sh` checks NIST
  and RFC 4231 vectors for both schedules before compiling.
- **`EVENTFD_WAKE=1`** (off): the other rings drop `DEFER_TASKRUN` and signal an eventfd that
  ring 0 polls, so a completion anywhere ends ring 0's wait. Latency halves, but ring 0 no
  longer waits out its 64-completion / 2 ms window: more, smaller batches, +18 % CPU per
  request. On one vCPU the CPU is what runs out at 290k+, so it stays off.
- **`IDLE_POLL=1`** (off): idle connections wait on a multishot poll and a poll event queues a
  single-shot read (as in c-iouring-pavlyhalim): less kernel memory per idle socket, one more
  submission per request. #479 had it on and its latency rose at 61 % CPU.
- **`BACKLOG_SLICE_US`** (0, off): a soft time limit on answering a large ready backlog.
- **`RECV_SIZE`** (1024): provided receive buffer size; 512 saves ~28 MB over seven rings.
- `LTO=1 bash build.sh` adds link-time optimization (off; the server and SQLite are already one
  translation unit).


## Follow-up to #482: 2,048-answer cap, 320K-user comparison

This follow-up changes **only** the default event-loop response budget:
`ANSWERS_PER_PASS=2048` instead of `16384`. The variable's environment
override is retained. The read budget (`READS_PER_PASS=16384`), per-ring
receive buffer count and size, ring waits (`WAIT_MIN=64`, `WAIT_US=2000`),
checkpoint schedule, PGO build, rolling SHA-256, multishot receives and
acceptance policy are unchanged from #482. The purpose is to measure
whether shorter response-processing passes protect tail latency at high
connection counts, not to claim an already achieved 320K result.

### Correctness

Run the challenge's unmodified 42-check suite on the new commit:

```bash
bash test/run.sh submissions/c-iouring-jowinjestine
```

Do not evaluate capacity unless all **42/42** checks pass, including
write/commit visibility and JWT verification. The GitHub pull-request CI
runs that same suite.

### Reproducible load A/B (separate k6 host)

Build once with the existing PGO setup. Use the same server/VM, kernel,
seed-database copy, JWT secret, process startup settings, k6 host and k6
workload for **both** runs. Start each run from a fresh equivalent seed copy
and restart the server, so the first variant cannot inherit the second
variant's WAL/database state. Avoid running k6 on the one-vCPU server.

- Control (#482): `ANSWERS_PER_PASS=16384`
- Experiment (this PR, default): `ANSWERS_PER_PASS=2048`

The canonical test is `bench/load.js` from the repository root. Its
ramp-up is 60 seconds; `DURATION` specifies the **hold**, not the ramp.

```bash
# Optional scouting probe: a 2-minute 320K hold, not an official score
k6 run -e BASE_URL=http://SERVER_IP:80 -e VUS=320000 -e RAMP_UP=60s -e DURATION=2m bench/load.js

# Required A/B comparison: a full 5-minute 320K-user hold, per variant
k6 run -e BASE_URL=http://SERVER_IP:80 -e VUS=320000 -e RAMP_UP=60s -e DURATION=5m bench/load.js
```

Record actual held VUs, request rate, p95, p99, failed request rate,
CPU microseconds/request, cgroup RAM peak, `max_turn`, `max_commit`,
`ready_max`, `rq_max`, `enobufs`, and checkpoint holds. Collect
per-10-second p99/queue trends where possible: a short probe can pass
while the five-minute hold degrades.

| Configuration | 320K 2-minute probe | 320K 5-minute hold | p95/p99 | errors | µs/req | RAM |
|---|---|---|---|---|---|---|
| #482, 16,384 answers | Not run | Not run | — | — | — | — |
| This PR, 2,048 answers | Not run | Not run | — | — | — | — |

A scored pass requires p95 **below 500 ms**, p99 **below 1,000 ms**
and failed requests **below 1%** on the entire five-minute hold.
An official result also requires the challenge operator's evaluation
on the specified 1-vCPU/2-GB environment. A 320K probe failure should
not be mistaken for a five-minute score.


## Experiment: TCP receive-memory governor (independent follow-up to #484)

This is enabled by default in this experiment (`TCP_MEM_GOVERNOR=1`), while `TCP_MEM_GOVERNOR=0` restores #484; it does not alter the
event-loop scheduler, B-tree reads or checkpoint policy. With
`TCP_MEM_GOVERNOR=1`, set `SO_RCVBUF=16384` on the eight listening sockets
before accepting connections. Linux doubles the requested value internally.
Every two seconds the server samples cgroup-v2 `memory.current`,
`memory.max`, `memory.stat` (`sock`), `memory.pressure` (PSI `avg10`),
and system-wide `/proc/net/sockstat` for **telemetry**, not an inferred
per-connection allocation.

After two consecutive high-pressure samples (PSI full avg10 >=2%,
PSI some avg10 >=10%, or >85% of cgroup memory.max), change the listener
cap for **future accepts only** to `SO_RCVBUF=8192`; restore the 16384
requested cap only after four low-pressure samples. Existing sockets,
connection admission, authentication, transaction semantics and responses
are untouched. The kernel may clamp requested values to `rmem_max`, and
accepted sockets inherit the listener's current buffer setting.
If cgroup v2 PSI is unavailable, the memory.current/max threshold can
still be used; missing metrics are zero rather than grounds to throttle.

Control: `TCP_MEM_GOVERNOR=0` (#484). The official run uses default-on. Test with
`TCP_MEM_GOVERNOR=1 TCP_RCVBUF_NORMAL=16384 TCP_RCVBUF_PRESSURE=8192`.
The log emits sampled memory, socket-accounted memory, PSI and current
listener cap. Compare 300K/310K/320K full 5-minute holds with the same
seed and PGO binary, preferably from a separate k6 host, and collect
p95/p99, errors, CPU/request, RAM, `enobufs`, retransmissions and TCP
memory. A smaller receive window may hurt throughput, so **do not enable
without an A/B win**. No performance claim until the official benchmark.

## Short kernel calls (follow-up to #486)

Two switches, both on: **`SUBMIT_CHUNK=128`** (a ring submits its queued operations at most 128 per
`io_uring_enter`) and **`ENTER_EVERY=128`** (a long pass enters every ring each 128 answers). Both are
there for one property of the box: its kernel is `PREEMPT_VOLUNTARY` ("Dynamic Preempt: voluntary"),
so a system call runs until it decides to stop, and on one vCPU everything else waits for it,
including ksoftirqd, which refills the NIC's 1,024-slot receive ring.

What that did at 300k, measured second by second on a box like the benchmark one (below):

- A batch's replies went to the kernel in one call per ring, and with `DEFER_TASKRUN` a ring's
  receives only run inside `io_uring_enter`, all of them in one call on 6.8. After any hiccup (the
  last seconds of the ramp, a checkpoint) those calls grew: the gap between two batches, normally
  ~6 ms, went to 77 ms, then 411 ms with 11,203 completions in one batch, and in #486's 5-minute
  runs up to 3.0-3.2 s.
- Meanwhile the receive ring overflowed on the host side, invisible to the guest, and clients lost
  packets: up to 109,000 retransmission timeouts per run on the load host (200 ms, 400, 800...).
- Requests that arrived during a long pass stayed in their sockets, each a 4 KB page against
  `tcp_mem`; past ~30,000 of them TCP went into memory pressure, and its queue pruning took the
  whole CPU for 5-10 s (the box stopped answering, even the 1-second sampler skipped seconds), with
  a 190,000-request backlog behind it.
- Every request answered more than 40 ms late also costs a delayed ACK from the kernel (20-38k per
  second during a stall, ~10 us each), which keeps the CPU saturated: the stall feeds itself.

With both switches the same hiccups stay hiccups: no call runs long and receives leave the
sockets every 128 answers. What is left is plain overload at the end of the ramp (~5,000 new
connections a second on top of the requests): a queue that drains as soon as the ramp ends,
without the freezes.

| 300k users, 60 s ramp + 5 min + 30 s down, p95 / p99 / max over the whole run | runs | p95 | p99 | max |
|---|---:|---|---|---|
| #486 (`5b6bfd3`) | 3 | 2.6-4.1 s | 6.6-7.6 s | 10.4-17.0 s |
| `ENTER_EVERY=128` only | 1 | 2.2 s | 3.4 s | 4.0 s |
| **both (this PR)** | 4 | 0.57-1.48 s | 0.99-2.65 s | 1.3-3.8 s |

The box: a Synteq 1 vCPU / 2 GB VPS like the benchmark one (Xeon Platinum 8260, Ubuntu 24.04,
kernel 6.8.0-117, 1,967 MB, no swap, no firewall), the app started by systemd as `deploy` with
`LimitNOFILE=65535` on a fresh copy of the seed for every run. The load came from a separate 4-vCPU
box in the same datacenter (0.18 ms) running a C port of `bench/load.js` (one keep-alive connection
per user, the same think times and request mix) over six address pairs. This box is harsher than
the benchmark box: #486 held 300,000 there but fails it here every time, so the absolute numbers
are not a prediction, only the difference is. Both switches can be turned off (`=0`).

Things measured on the way that did not help and are not in this PR: answering this batch's
arrivals before an existing backlog (fewer stalled seconds, but the backlog's own requests waited
up to 20 s), smaller pieces (`SUBMIT_CHUNK=32`, `ENTER_EVERY=64`), no 64 MB log read-ahead, 16 MB
checkpoint cycles, no periodic page drop or writeback kicks, a lower-priority event loop (nice 5/10).

## Newest first (follow-up to #486)

One change of order: **a pass answers what arrived since the last pass first, and the backlog
with what is left** (`FRESH_FIRST`, on). It was the other way round, oldest first. Two rules make
it exact: a pass takes the arrivals that can still be answered within the kernel's 40 ms, however
many that is, and the backlog waits while arrivals are still being sent to it. Three small things
go with it: a reply that is sent late puts its socket back into delayed-ACK mode
(`DELACK_RESTORE`, on), the listeners start in that mode (`QUICKACK_OFF`, on, as in
fsharp-uring-delneg #437), and the number of connections is limited by the box's memory
(`CONN_LIMIT`). No request is dropped: every one that is read is answered.

Why the order matters here. Linux acknowledges a request with its response if the response
leaves within 40 ms. If it does not, the delayed-ACK timer sends an ACK of its own and takes the
socket out of delayed-ACK ("pingpong") mode, so that connection's *next* request, seconds later,
is acknowledged on arrival with another packet of its own. A late reply therefore costs up to two
packets more than a reply in time, kernel work of the order of the reply itself. Oldest first,
one pause of the loop (a preemption, a disk wait, the last seconds of the ramp) makes every
request behind it late, each late request costs those packets, and above ~80 % of the CPU that
is more than the spare capacity: the queue stops draining, and latency settles at 1-2 s, where
the slowed-down clients finally send less. The failed runs on a box like the benchmark one (the
one of the section above) all looked like that: a failed 280k run sent 4.1
million segments that carried no data, a clean 300k run 1.6 million, and second by second the
server sent more packets than it received for as long as the queue lasted and for ten seconds
after it (the second ACKs).

Newest first, a pause costs the requests that were waiting when it ended, and no others:

- A pass is sized by that deadline, not by a count. The loop measures what an answer costs it (a
  pass and its enter, per answer: 25-30 µs at 300k users). A pass's n arrivals are spread over
  the time since the last pass, its `age`; the newest k of them all leave within `DEADLINE_MS`
  (32, the 40 ms with a margin) if `age * k / n + k * cost` fits in it. That many are answered
  now, all of them if it holds for n. An arrival stays in its receive buffer on its ring's
  "fresh" list until the pass has seen every ring; the ones that cannot make it move to the
  backlog in arrival order, from each ring in proportion. (The first version of this PR took 384
  a pass whatever they cost, `ANSWERS_PER_PASS=384`: a pass that started 3 ms late sent the tail
  of its arrivals to the backlog though they could still have been answered in time.)
- The backlog is answered oldest first with what the arrivals leave of a pass of `PASS_MS` (8 ms
  of work: half of what the deadline would allow, so that a pass that starts late still answers
  its arrivals in time), and only once no arrival has gone to it for `BACKLOG_QUIET_MS` (30).
  While arrivals do not fit their passes, a request answered from the backlog sends an arrival
  there in its place, in that pass or the next: the backlog is no shorter, one more request is
  late, and one more delayed ACK is paid. (The first version gave the backlog 24 answers of
  every pass: in the last seconds of the ramp at 320k, 92,000 requests went through a backlog
  that never held more than 36,000.) A lasting overload cannot starve it: once its oldest
  request has waited `BACKLOG_AGE_MS` (20 s) it gets `FRESH_RESERVE` (24) answers of every pass,
  and half of every pass after twice that, so nothing comes near a client's timeout.
- Backlog replies, writes released from a checkpoint hold, and everything a pass answers after
  the loop was away for more than `LATE_MS` (20) get one more operation behind their send:
  `setsockopt(TCP_QUICKACK, 0)` through `IORING_OP_URING_CMD` (a direct descriptor has no file
  descriptor to call it on), which puts the socket back into delayed-ACK mode. Its next request
  is then acknowledged by its response again.
- `TCP_QUICKACK` off on the listeners does the same for a connection's first request: one packet
  less per new connection, ~5,000 a second at the end of the ramp.
- With passes this short nothing needs reaping or entering mid-pass: `ENTER_EVERY` is off in
  this mode (`SUBMIT_CHUNK=128` stays).
- A waiting request keeps a 128-byte buffer, not 512 (a GET is ~100 bytes): with every
  connection waiting, that was 170 MB the box does not have (see `CONN_LIMIT` below).

This is the "adaptive LIFO" of overloaded request queues (Maurer, *Fail at Scale*, ACM Queue
2015), with the kernel's delayed-ACK timer as the deadline.

Measured on that box with the first version (`1ac444a`: 384 answers a pass, 24 of them for the
backlog), whole-run percentiles like k6's thresholds:

| 300k users, 60 s ramp + 4 min; the server process stopped (SIGSTOP) for 150 ms at 150 s and for 300 ms at 220 s | p95 | p99 | 10-second windows after the 300 ms pause (p95) |
|---|---|---|---|
| #505 (`29cfa02`, oldest first) | 221.5 ms | 732.8 ms | 678 ms, 1,008 ms, 189 ms, 325 ms, then normal; again 332 ms 80 s later |
| first version | 10.8 ms | 33.4 ms | 21 ms, 163 ms, 15 ms, then normal |

The pause leaves ~15,000 requests waiting; here they are answered within 2 s and nobody else
waits, there every request of the next 40 s waited behind them. The end of the ramp, a pause of
its own kind, went from p95 198-329 ms for 20 s to 17 ms.

| 320k users, 60 s ramp + 5 min + 30 s down | p95 | p99 | max | failed |
|---|---|---|---|---|
| #505 | 3.7 s | 5.5 s | | 0 |
| first version | 17.5 ms | 404.5 ms | 5.7 s | 0 |

That run is the first 320k one this entry has passed on that box. What is left in it is the last
ten seconds of the ramp, where the new connections (~5,300 a second) and the requests together
need more than the CPU: the backlog reaches 36,000 there, requests wait up to 5.7 s, and the hold
is clean. Those seconds are what the two rules above (passes sized by the deadline, a backlog
that waits) are for: they cost 92,000 late requests where 36,000 would have paid the same debt,
and a 2-minute probe at 320k has room for ~57,000 requests over 1 s before its p99 fails.

That box has been down since the 340k run below, so the two rules were measured on a KVM guest on
my PC instead: the benchmark box's kernel (6.8.0-117) and memory (1,967 MB), one vCPU capped at a
share of a host core (68 % puts 320k users at ~85 % of it), load from the host over a tap device,
a fresh boot before every run, the versions interleaved. "First version" is the first commit's
behaviour (`ANSWERS_PER_PASS=384 BACKLOG_AGE_MS=0 BACKLOG_QUIET_MS=0`). All runs are 320k users,
60 s ramp + 2 min + 30 s down, the shape of a probe.

Nothing injected; what goes wrong is the end of the ramp and the guest's own hiccups:

| cap | | p95 | p99 | requests over 1 s | sent to the backlog | delayed ACKs |
|---|---|---|---|---|---|---|
| 68 % | first version | 20.3 / 22.3 / 10.7 ms | 746 / 370 / 19 ms | 37,824 / 41,541 / 0 | 129,687 / 167,812 / 18,586 | 162,788 / 188,465 / 59,205 |
| 68 % | **this PR** | 10.7 / 11.2 ms | 20.4 / 21.0 ms | 0 / 0 | 1,555 / 1,449 | 52,263 / 52,199 |
| 64 % | first version | 19.3 / 14.3 ms | 206 / 29 ms | 11,701 / 0 | 115,383 / 44,615 | 145,035 / 68,188 |
| 64 % | **this PR**, `PASS_MS=16` | 16.5 / 16.7 ms | 39 / 38 ms | 0 / 0 | 17,835 / 17,678 | 76,359 / 81,799 |

An overload made on purpose 35 s into the hold (the cap drops from 68 %):

| the cap drops to | | p99 | requests over 1 s | sent to the backlog | |
|---|---|---|---|---|---|
| 50 % for 10 s | first version, four runs | 2.5-4.6 s | 118,165-234,363 | 181,855-318,629 | all fail |
| 50 % for 10 s | **this PR**, two runs | 236 / 514 ms | 52,418 / 55,541 | 59,939 / 59,756 | both pass |
| 54 % for 6 s | first version, two runs | 225 / 692 ms | 59 / 30,567 | 104,171 / 129,805 | both pass |
| 54 % for 6 s | **this PR**, `PASS_MS=16`, two runs | 39 / 40 ms | 6,348 / 2,168 | 28,296 / 24,395 | both pass |

The first version turns a small backlog over fast enough to keep most of it under a second, at
the price of four times as many late requests; past a certain depth that stops working and the
late requests are all it has to show. This PR pays the debt once.

And an overload that lasts, 320k at a cap of 46 %, a quarter more than the guest can serve, for
the whole run (`PASS_MS=16`): no request failed or timed out with either version. The first
version answered 24 % of the requests after more than 1 s (longest wait 15.9 s), this one 16 %
(22.1 s), with 60,000-80,000 requests waiting at any moment and ~200 MB of page cache left
throughout.

Not every run went well. The guest has pauses of its own, 0.5-2 s in which the server gets no
CPU, in some runs and not in others; they are why single runs differ so much. In two runs of this
PR (one of them the third of the 68 % rows above, which is why that row has two numbers) such a
pause left more requests unread than the guest's memory holds, and the kernel ended the server
(the collapse described under `CONN_LIMIT` below). The first version met pauses as long in other
runs and came through: two cases in thirteen runs against none in fifteen. In both of those runs
the guest was losing a fifth of its time to the host (3-8 % in the other runs), so I take them
for the host's doing, but I cannot prove it, and say it for that reason.

`CONN_LIMIT`: the test box at 340k stopped answering on the network 200 s into the run and had to
be restarted. The guest shows why. A connection costs the kernel 4.13 KB (the TCP socket, its
inode, dentry and file, the armed receive), so 340,000 are 1.4 GB on 1,967 MB, and what is left
is ~100 MB free and ~150 MB of page cache. That is enough for a quiet run (340k at a reduced
request rate held for four minutes) and not for a burst. A request that arrives while the server
is not running waits in its socket and pins ~3.5 KB there (its slice of one of the network
driver's 32 KB receive pages), so at 320k the box has room for about two seconds' worth of
requests, and at 340k for one. Past that the kernel's OOM killer ends the server; if packets
keep coming it ends every other process next and then gives up ("Kernel panic - not syncing:
System is deadlocked on memory"), which from outside is a box that no longer answers. The end of
a run is such a burst too, every connection closing at once with an unread FIN in each: on the
guest the server was killed there, after the measurement, in the runs at 320k and above, and not
at 310k.

No server setting changes what a connection or an unread request costs the kernel, so the server
holds at most (MemTotal - 600 MB) / 4.25 KB connections, 329,000 on that box. At the limit the
listen queues go down to one entry: the kernel drops the SYNs of the users over it, who wait in
`connect()`, and does not keep thousands of established sockets nobody will accept. A connection
that still gets through is answered `503` and closed. Checked with a limit of 50 and 120
clients: 50 served, 70 answered 503, and 30 new ones served after 30 of the first left.

So the users over the limit mostly wait rather than fail, like the users over 65,535 of the
entries in the leaderboard's footnote on the descriptor limit, and a load a few percent over
the limit can pass on errors: in two rehearsals at 340k the 330,000 that were served were
answered as usual (p99 45 and 68 ms) and 0.72 % and 0.89 % of the requests failed. Refusing every connection over the
limit with a 503, so that each one counts, was tried and taken out again: accepting a connection
to refuse it costs what accepting it to serve it costs, at the moment the box has least to
spare. That rehearsal refused 20,000 in the seconds after the limit was reached, and the 330,000
being served waited up to 28 s behind it.

**What the limit does not do.** On the guest (cap 66 %) I rehearsed the probes that would follow
a passed 320k. A little over the limit, 340k, twice: one rehearsal ran to its end; in the other
the kernel ended the server two seconds before the end of the hold (one of the guest's pauses,
with 330,000 connections and a second of room) and the guest then panicked. At 400k and 640k,
with 330,000 connections held and tens or hundreds of thousands of users retrying, packet
processing takes the CPU, the server gets none for seconds at a time, memory runs out as described above, and in all
five rehearsals the kernel ended the server; in three of them (400k once, 640k twice) it then
ended everything else and panicked too. A server that held few enough connections to live through
that, ~240,000 on this memory, could not hold 320,000 in the probe before it. So if 320k passes
on the benchmark box, the probes far above it may take the box down, with this server or any
other that holds that many connections.

As in the section above, the test box and the guest are harsher than the benchmark box and the
numbers are a comparison, not a prediction. `FRESH_FIRST=0` restores the old order (with
`ANSWERS_PER_PASS=2048` and `ENTER_EVERY=128`); `ANSWERS_PER_PASS=384 BACKLOG_AGE_MS=0
BACKLOG_QUIET_MS=0` is the first version of this PR; `DELACK_RESTORE=0`, `QUICKACK_OFF=0` and
`CONN_LIMIT=0` turn off the other three.
