# Design

A single-threaded C server on io_uring with SQLite in-process, built to hold as
many idle keep-alive connections as a 1 vCPU / 2 GB box has kernel memory for,
and to spend as little CPU per request as the SQLite work allows. Every decision
below names the rule it respects.

## Layout

| Module | Job |
|---|---|
| `src/net.c` | io_uring event loop, connection table, buffers, batching, memory guard |
| `src/app.c` | routing, auth, validation, response bodies, the batch (group commit) protocol |
| `src/db.c` | SQLite connection, prepared statements, feed strategies, checkpoint thread, write hold |
| `src/db_btree.c` | direct B-tree cursor reads for the feed and single posts (same translation unit as the amalgamation) |
| `src/http.c` | request parser (in place, no allocation), response head |
| `src/jwt.c`, `src/crypto.c` | HS256 verification, SHA-256/HMAC, base64url |
| `src/json.c` | validating JSON scanner for the two documents we read, string escaper |
| `src/config.c` | SPEC environment variables plus tunables with defaults for the target box |

Dependencies: SQLite 3.54.1 amalgamation and liburing 2.15, fetched by
`fetch-deps.sh` with checksum verification (SHA3-256 from sqlite.org, SHA-256 of
the liburing release tarball). Nothing prebuilt is committed (rule 10).

## The event loop (`net.c`)

**One thread.** With one vCPU a second thread only adds context switches. The
only other thread is the checkpointer, and it exists to move `fsync` waits off
the loop, not CPU.

**Rings and direct descriptors.** The kernel caps each io_uring fixed-file
table at the *hard* `RLIMIT_NOFILE` (measured on Ubuntu 6.8: with the hard limit
at 65,535, `register_files_sparse(65536)` fails with EMFILE; with a 1M hard
limit, 200k slots register fine even with a 1,024 soft limit). The benchmark
service gets `LimitNOFILE=65535`, so connections live in direct descriptors
spread over `ceil(max_conns / 65535)` rings, 7 rings for the default 450k table.
Accepted sockets never become process file descriptors, which is the only legal
way past the limit without closing clients' connections (rule 11 forbids
raising it for us).

Ring 0 is where the thread blocks (`io_uring_submit_and_wait_timeout`); it
uses `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | SUBMIT_ALL`. Every pass
reaps every ring. A secondary ring also wakes ring 0 through a *one-shot*
`POLL_ADD` on its fd, armed only after the ring was drained: a multishot poll
fires again on every pass while the CQ holds anything, which is what kept the
scored version from ever waiting for a batch once several rings were busy. Those rings use
`SINGLE_ISSUER | COOP_TASKRUN | SUBMIT_ALL` **without** `DEFER_TASKRUN`: a
deferred ring posts its CQEs only when its own task work runs, and only a
posted CQE wakes the poll (queued-but-deferred work does not), so a deferred
secondary ring went silent after its first event in testing. With
`COOP_TASKRUN` their work runs whenever the thread crosses the kernel boundary,
including inside ring 0's wait. Both have a fallback chain to plainer flags on
older kernels. The poll must be re-armed whenever a CQE arrives without
`IORING_CQE_F_MORE`, which happens.

**Accept.** Exactly one ring accepts at a time, the one with the most free
slots. While it has more than 4,096 free slots (slots/4 for smaller tables) it
runs a multishot accept (`IORING_FILE_INDEX_ALLOC`). A multishot accept that
finds the table full fails with `-ENFILE` *after* accepting the socket, which
the kernel then drops, and it drains a deep backlog faster than a cancel can
land, so for the last 4,096 slots the ring switches to one-shot accepts, at most
64 in flight and never more than the free slots; tables of 1,024 slots or less
use one-shots only. When the ring is full the next one takes over; when every
table is full nothing is armed and the listen backlog (4,096, the kernel
default `somaxconn`) holds clients until a close frees a slot.

**Receive.** One multishot `RECV` per connection with `IOSQE_BUFFER_SELECT`
from a pool of provided buffers per ring (2,048 x 1 KiB; requests are a few
hundred bytes and a POST body spans buffers). An idle connection
owns no buffer. Classic `IORING_OP_PROVIDE_BUFFERS` is used, not the newer
buffer ring: this kernel rejects `IORING_REGISTER_PBUF_RING` with EINVAL
(verified with a raw syscall and both struct layouts), and one tested path beats
an untested fallback on the scoring box. Returned buffers are coalesced into
runs and given back with one SQE per run.

**ENOBUFS.** When the pool is empty a receive completes with `-ENOBUFS` and
the multishot ends. The connection goes into a per-ring FIFO and is re-armed
only at the end of an iteration, after buffers were returned, and only as many
as `max(returned, 32)` (capped at half the pool). Re-arming immediately is the
"ENOBUFS storm" that cost the NASM entry 37% failures at 250k.

**Bounded work.** Each iteration drains every ready CQ (cheap: receives only
record which buffer holds which connection's bytes), then handles at most
`work_budget` (512) requests, then submits all sends. Send and close completions
are therefore processed before any new request in the next iteration frees
their resources ("send-first ordering").

**Batching.** After an iteration with 4 or more completions the loop waits for
512 CQEs or 16 ms, whichever first (see "Batch window trade-off" below); otherwise it waits for 1 CQE (50 ms timeout
for the periodic tasks). Only requests already queued (a batch that hit its
budget) skip the wait; completions sitting in a secondary ring's CQ do not,
because each pass costs an `io_uring_enter` per ring plus a read and a write
transaction, and the official run showed passes of a few requests at 20k
req/s over four rings (38 us/request against 27-30 us with passes of 30). The
F# and C entries ahead on the leaderboard pace at 1-2 ms; the F# author
measured -9.9% CPU for the window alone. The scored version used 250 us; the
second version 32 CQEs or 2 ms.

**Connection record.** 48 bytes: generation, state, flags, ring, slot, last
activity, send chunk, parked receive buffer (id/offset/length), and a pointer
to a heap copy that only exists while a request is incomplete. Generations tag
every SQE so a completion for a slot that was closed and re-used is ignored.

**Send buffers.** A bounded pool of 1,024 x 72 KiB chunks (72 MiB of address space,
faulted in lazily; a few dozen are in flight at 35k req/s, and a pass of up to
512 requests needs up to 512 at once);
72 KiB holds the largest legal feed (20 posts of 500 characters that each
escape to 6 bytes, plus the envelope), and the writers check the actual
escaped length rather than a worst-case bound. A response is written straight
into a chunk, the head (status, `Content-Type`, `Content-Length`, `Connection`
when it is not the version's default, `Date` cached per second) is placed right
before the body, and the chunk is returned on the send completion (short sends
resend the remainder). When the pool is empty the connection parks with its
input until a chunk frees up; nothing is dropped. A peer that stops reading
pins its chunk only until the 30 s sweep closes it.

**Pipelining and partial requests.** One request per connection per batch;
remaining bytes stay parked until the send completes, then the connection is
re-queued. Incomplete requests move into a per-connection heap buffer (sized to
the bytes held, growing to a 24 KiB cap: 8 KiB head + 16 KiB body) so the
shared receive buffer is not held. A peer that half-closes right after its
request (data and FIN in one batch) still gets its response, then the close. Headers
over 8 KiB answer 413 and close; bodies over 16 KiB too. `Expect: 100-continue`
gets its interim response. Connections that leave a request incomplete for 30 s
are closed by a 10 s sweep; idle keep-alives are closed after 60 s (default
`--idle-timeout-s`; the direct k6 users idle up to 15 s plus latency).

**Dead peers.** `SO_KEEPALIVE` with a 120 s idle / 30 s interval / 4 probes is
set on the listener and inherited, so a peer that vanished without a FIN frees
its slot after about four minutes instead of never.

**Closing.** `shutdown(SHUT_RDWR)` first so the armed multishot receive
terminates, then `close_direct` once both the receive's final completion and
the shutdown's completion have arrived (see "Close-path race" below). A send
still in flight keeps its chunk until it completes; the generation check frees
it.

**Memory guard** (rule 8: run out of memory and you lose). Every 100 ms the
loop computes available memory: `MemAvailable` minus the growth of
`SReclaimable` since start (reclaimable dentry and inode cache from sockets is
counted as available by the kernel but is not free in practice) and, when the
process runs under a cgroup with `memory.max`, the cgroup headroom if lower.
The cgroup headroom is `memory.max` minus anon + kernel + sock from
`memory.stat` (falling back to `memory.current` if `memory.stat` cannot be
read). Earlier versions used `memory.current`, which includes page cache; under
a limit the cache fills to the limit by itself and is reclaimed on demand, so
the guard saw almost no headroom and shed or paused accepting when memory was
not short (RESULTS.md section 9).
The value is smoothed over about 1 s. Its effects are described under
"Load shedding" below. Accepting pauses only below 96 MB available (resuming
above that with hysteresis), and keep-alives idle longer than 60 s are closed.
The previous version paused accepting at 256 MB and closed idle connections below
128 MB; the second scored run showed that a pause cannot save a run (see "Load
shedding" below).

**Prefault.** Off by default: nothing is touched at startup beyond what the
first requests need, so resident memory is only what is used. `--prefault`
touches the connection table, send pool and receive pools and warms the SQLite
page cache, for runs where first-request page faults matter more than memory.

**TCP_NODELAY** is set on the listening socket and inherited by accepted sockets
(verified); direct descriptors cannot call `setsockopt`. Without it the last
partial segment of a feed response would wait on the client's delayed ACK.

## SQLite (`db.c`)

- **Rule 2:** the amalgamation is linked in; `SQLITE_PATH` is the only store.
- **Rule 5:** every request runs its statements; no result, response or token
  verdict is remembered between requests. Prepared statements are reused
  (explicitly allowed). The JWT signature is recomputed on every authenticated
  request.
- **Rule 6:** `journal_mode=WAL`, `synchronous=NORMAL`. Writes of one batch
  share one `BEGIN IMMEDIATE ... COMMIT`; `app_end_batch()` returns only after
  `COMMIT` succeeded, and the loop submits sends after that. If the commit
  fails every write response of the batch becomes a 500. A statement error that
  rolls the transaction back fails the whole batch too (`sqlite3_get_autocommit`
  is checked), so no 201 can describe an uncommitted row.
- **Reads in a batch** share one deferred `BEGIN ... COMMIT` that ends before
  the write transaction begins. Reads never see uncommitted rows of the same
  batch; they see the committed state at batch start, which is a legal
  linearization since no write of the batch has been acknowledged yet.
- `unix-excl` VFS: an exclusive lock against other processes with the WAL index
  in heap memory shared by our two connections, instead of `fcntl` locks and an
  mmap'd `-shm` on every transaction. `locking_mode=EXCLUSIVE` would have
  blocked the checkpoint connection.
- `mmap_size=0` (compiled out with `SQLITE_MAX_MMAP_SIZE=0`): with mmap the feed
  went from 43 to 290 us once the WAL held 40k frames in the exploration
  measurements, and anonymous cache pages cannot turn into major faults when
  socket memory squeezes the page cache.
- `cache_size=-4096` (4 MiB; 32 MiB gave the same CPU and 10 MB more anonymous
  memory, RESULTS.md section 8), `temp_store=MEMORY`, `wal_autocheckpoint=0`,
  `journal_size_limit=64 MiB`.
- Compile: `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DQS=0`, `OMIT_DEPRECATED`,
  `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_PROGRESS_CALLBACK`,
  `OMIT_DECLTYPE`, `OMIT_AUTOINIT`, `USE_ALLOCA`, `LIKE_DOESNT_MATCH_BLOBS`,
  `MAX_EXPR_DEPTH=0`, `-O3 -march=native -flto`. PGO (`make pgo`, trained on
  a synthetic database) is part of `build.sh` and falls back to the plain LTO
  build if training fails; on the laptop it measured no CPU gain (RESULTS.md
  section 2).

  Why not `THREADSAFE=0`: the checkpoint thread needs a second connection, and
  with all mutexes compiled out the per-inode lock bookkeeping that both
  connections share (`unixInodeInfo`, the heap WAL index locks) is unprotected.
  `THREADSAFE=2` keeps the core mutexes (uncontended, a few per transaction)
  and drops the per-connection mutex, which is the expensive one.

**Feed as three range scans (SQL path).** This is the fallback for the B-tree
reads below. The 20 newest `(created_at, id)` come from the
covering index. If the ids span at most 256, one range scan of
`likes_post_id_idx` (one row per like, tallied in C: about 30% cheaper than
`GROUP BY count(*)` for the few hundred likes a feed spans) and one range scan
of `posts JOIN users` over the span replace 20 + 20 + 20 point lookups. Rows are copied into
a scratch area and emitted in the index order. Otherwise, or on any anomaly,
the reference query runs. `--feed-reference` forces the reference query; the
black-box suite checks both strategies produce identical feeds. Every value is
read on the request that serves it (rule 5).

**Checkpoints off the loop.** A second connection on a nice-10 thread runs
`PASSIVE` checkpoints whenever the WAL (reported by the WAL hook after every
commit) exceeds 1,000 frames not yet backfilled, so the loop never waits for an `fsync`. PASSIVE
checkpoints alone never let the WAL restart under continuous writes (the Zig
entry saw 5 MB/s of unbounded growth). So when the WAL reaches 8,192 frames
(32 MiB) the loop *holds writes* (reads continue; held requests park with their
input) until the checkpointer reports the WAL fully backfilled; the next write
transaction then restarts the WAL from frame 1. A hold gives up after 500 ms
and retries 8,192 frames later. (The scored version held at 8,192, the second
version at 16,384, which halves the holds because a hold lasts as long as the
checkpoint's fsyncs, not its copying; the current 8,192 with a 1,000-frame
checkpoint trigger measured 145 MB cgroup memory against 174 MB.) Holds are counted and timed in the stats line.

**Create** uses `INSERT` then `SELECT created_at` by rowid (the column default
`strftime` produces the timestamp, as the SPEC describes). **Like** checks the
post exists, then `INSERT ... ON CONFLICT DO NOTHING`; `sqlite3_changes()`
decides 201 or 200.

## B-tree reads (`db_btree.c`)

The feed and `GET /posts/:id` read through SQLite's B-tree cursor API instead
of the SQL layer. The file is included in the same translation unit as the
amalgamation, so it can use SQLite's internal functions. Per feed request:

1. The newest 20 ids come from the created-at index (cursor from the end).
2. One range scan of the likes index over the id span, tallied in C.
3. The post rows and their author rows are found by rowid seek.
4. The JSON is written straight from the page bytes of those records.

A single post is one rowid seek plus its author. Writes stay prepared SQL.

*Rule 5.* Every value is read from SQLite's pages on the request that serves it,
inside the batch's read transaction. No row, id list, count or response is kept
between requests; the only things reused are the open cursors' schema facts
(root pages, column positions), which are checked rather than assumed.

*Validation.* At open the code checks the schema shape (tables, columns, index
definitions) and the schema cookie. The cookie is compared again for each
batch's read transaction, so a schema change after startup is noticed.

*Fallback.* Any record the direct reader does not handle sends that request to
the prepared SQL path: an overflow payload, fewer than 20 rows, an id span over
1,024, a missing author row, a changed schema. `DB_READS=sql` turns the direct
path off entirely. The code depends on SQLite 3.54.1 internals; the
amalgamation is pinned by checksum.

*Measured* (RESULTS.md section 8): in a SQLite-only microbenchmark the feed
costs 14.2 and 14.4 us against 32.8 and 30.3 us for the SQL path; a single post
3.6 and 3.4 us against 4.2 and 4.4 us. `tools/dbdiff.c` found the feed and all
500,005 post ids byte for byte identical, before and after random writes.

*Open question.* Rule 2 says the database is SQLite used in-process. This reads
SQLite's own file format through SQLite's own code, but it is a different API
from SQL. The same technique is used by lean-derive-hargup (#153) and
c-iouring-public (#158), and the organizer's rulings on those entries apply to
this one.

## Load shedding

*What.* Each response carries `Connection: close` with a deterministic fraction
f (an 16.16 fixed-point accumulator, no random draw). The client closes its socket
and reconnects for its next request. With f near 1 a user holds no socket
during its think time.

*Why.* The kernel's memory per established socket is the limit on the box, not
this process. Measured on kernel 6.8.0-147 over loopback, an idle established
TCP socket costs about 2.5 to 2.6 KB charged to the cgroup (about 1.45 KB
unreclaimable slab and about 1.05 KB reclaimable dentry and inode cache) and
3.6 to 3.9 KB system-wide, averaged over both endpoints; socket buffers are 0
while idle. The organizer's estimate for the box is about 4.3 KB. Receive
buffer sizes of 4,096, io_uring fixed files and poll arming changed nothing
useful (a multishot poll also pins a socket after `close_direct` unless it is
cancelled). So to serve more users than the box has socket memory for, users
must outnumber sockets.

*Status: the laws are OFF by default.* After the scored run of #228 (228,000
held, 240,000 failed with connection refused) local cgroup tests showed that
the shedding laws were driven by an input that counted page cache and that
closing and reconnecting was net harmful (RESULTS.md section 9), so
`--shed-hi-mb`, `--shed-lo-mb` and `--conn-cap` now default to 0 (law off). The
flags remain and re-enable the laws as described below.

*The two laws.* f is the larger of:

- a memory law: 0 at or above `--shed-hi-mb` available (for example 450), 1 at
  or below `--shed-lo-mb` (for example 200), linear between, on the available
  value described under "Memory guard";
- a connection-count law: 0 at 90% of `--conn-cap`, 1 at the cap (for example
  260,000), which acts before memory falls.

Only the idle-keep-alive sweep is rate-limited (a burst of 512 per pass,
refilled at 200 per ms) so that a mass idle close does not push slab up in one
step; a close of 20,000 sockets raised slab by at most about 16 to 25 MB for
0.1 to 0.3 s. Per-response closes (the shed fraction itself) are not
rate-limited. `--shed-force F` fixes the fraction for tests.

`--hard-conn-cap N` (default off) is a separate, optional guard: when N
connections are open the server stops accepting and resumes once enough have
closed; clients wait in the listen backlog.

*Why not an accept pause.* In the second scored run the 240,000 probe paused
accepting twice (252 MB available) and failed on latency (p95 5.8 s): a pause
leaves established sockets in place, so memory does not fall, and the users
waiting in the backlog time out. Shedding releases the sockets that idle users
hold, which is the memory that matters. The pause remains as a last resort at
96 MB.

*Reconnect cost.* A reconnect is accept, handshake, shutdown and close, almost
all kernel time. Locally (earlier build, 10,000 users): F=0 23.15 us/request,
F=0.2 30.85, F=0.5 37.3, F=1.0 45.5, i.e. about 22 us per reconnect at F=1.0,
28 at 0.5 and 38 at 0.2 (the marginal cost is highest at low F). Linking the
shutdown and `close_direct` saved under 1 us and was not kept; cancelling the
receive before closing is invalid because a fixed-file close does not release
the socket while a receive is armed; `TCP_DEFER_ACCEPT` gave no gain.

*Reading the stats line.* The server prints a stats line to stderr every 10 s
(`--stats-interval-s 0` disables it). It carries open connections, requests,
cpu_us/req (user and system), rss, `shed_f`, `shed_closes`, `idle_closes`,
accepts and the accept increment per interval, `avail_mb`, WAL frames, holds
and `btree_fb`. To tell memory pressure from a CPU limit: if `avail_mb` is
falling toward 450 or below and `shed_f` is above 0 while cpu_us/req times the
request rate is well under one core, memory (or the connection cap, when
connections are near `--conn-cap` but `avail_mb` is high) is the limit. If
`shed_f` stays 0, `avail_mb` is comfortable and cpu_us/req times req/s
approaches 1,000,000 us per second, the CPU is the limit. A rising
accept increment with a high `shed_f` is the reconnect cost showing up as
system time. A nonzero `btree_fb` means requests are falling back to SQL.

This is general memory protection for any client. It does not look at who the
client is, and HTTP clients reconnect transparently after `Connection: close`.

## Close-path race

A shutdown request runs on a kernel worker thread and looks its fixed-file slot
up when it runs. The first version closed the slot as soon as the receive
completed and submitted the shutdown; the next accept could reuse the slot
before the worker ran, and the late shutdown then killed the new client's
socket. With reconnecting clients the original server lost the first request of
25 of 300 clients; the fixed server lost 0 of 3,000. A slot close now waits for
both the receive's final completion and the shutdown's completion. A regression
test is in `test/http_tests.py`.

## Batch window trade-off

A longer window gives larger batches, so fewer passes, transactions and
`io_uring_enter` calls per request, at the price of latency. Laptop numbers
(10,000 users, think-scale 0.2, 40 s; us per request at F=0 and F=0.3):

| Window (CQEs / time) | F=0 | F=0.3 | p95 |
|---|---|---|---|
| 32 / 2 ms (previous) | 22.7-23.5 | 32.4-34.2 | 3-4 ms |
| 128 / 4 ms | 21.0-21.4 | 28.3-30.2 | 5-9 ms |
| 256 / 8 ms | 21.1-21.2 | 27.9-29.0 | 10-18 ms |
| 512 / 16 ms (adopted) | 20.7 | 27.2-28.3 | 20-35 ms |

So the adopted window costs up to about 35 ms of p95 on the laptop for 10 to 15
percent less CPU. 1,024 / 32 ms was not adopted: its shedding runs were disturbed
by machine load. The latency is only paid when the loop is busy enough to fill
or wait out the window; an idle loop wakes on the first completion.

## HTTP, JWT, JSON

- The parser works in place on the receive buffer: method, target, version,
  then the five headers we care about by length and case-insensitive compare.
  Bare LF line endings are tolerated, folded headers and `Transfer-Encoding`
  are rejected (400), duplicate or non-numeric `Content-Length` is a 400.
- JWT: split on dots, base64url (canonical: no padding, zero leftover bits),
  HMAC-SHA256 with the inner/outer pad states precomputed once, constant-time
  compare, then the header must say `HS256`, then `exp` must be a number in the
  future (no `exp` is rejected, as the organizer's review asked), then `sub`
  must be a positive-integer string and `username` a string. A portable SHA-256
  is used; SHA-NI was not added because auth is 7.8% of requests and the
  exploration put the gain at ~0.06 us per average request.
- JSON: a validating recursive-descent scanner (depth 32) that extracts the
  top-level string members we need and unescapes them, including surrogate
  pairs. Output escapes `"`, `\`, and control characters; non-ASCII passes
  through (allowed by the SPEC).
- Body rules: trim ASCII whitespace, then count UTF-8 code points exactly as
  SQLite's `length()` would for well-formed text: empty is "body is required",
  more than 500 is "body must be at most 500 characters". Invalid UTF-8 is
  "malformed JSON body" (JSON is UTF-8) and a NUL is "body is required"
  (SQLite's `length()` stops there), so the schema CHECK can never fail.
- A query string is ignored for routing (`/feed?page=2` is the feed); a
  post id with leading zeros (`007`) is the number 7, like `Number()`.

## What was deliberately not done

- No response, query-result or token-verdict caching of any kind (rule 5).
- No assembly: the measured hot spots are the kernel TCP path and SQLite's VDBE,
  which C expresses as well as asm, and the exploration found hand-written AVX2
  no faster than intrinsics for JSON.
- No accept pause as the main memory defence: shedding with
  `Connection: close` replaces it (below).
- No SQPOLL, busy polling, zero-copy send or send bundles (negative or
  unavailable on 1 vCPU / kernel 6.8).
