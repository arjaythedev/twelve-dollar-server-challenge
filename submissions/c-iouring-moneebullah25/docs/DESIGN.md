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
from a pool of provided buffers per ring (4,096 x 2 KiB; the F# entry found a
saturated loop running dry at 2,048). An idle connection
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
`work_budget` (256) requests, then submits all sends. Send and close completions
are therefore processed before any new request in the next iteration frees
their resources ("send-first ordering").

**Batching.** After an iteration with 4 or more completions the loop waits for
32 CQEs or 2 ms, whichever first; otherwise it waits for 1 CQE (50 ms timeout
for the periodic tasks). Only requests already queued (a batch that hit its
budget) skip the wait; completions sitting in a secondary ring's CQ do not,
because each pass costs an `io_uring_enter` per ring plus a read and a write
transaction, and the official run showed passes of a few requests at 20k
req/s over four rings (38 us/request against 27-30 us with passes of 30). The
F# and C entries ahead on the leaderboard pace at 1-2 ms; the F# author
measured -9.9% CPU for the window alone. The scored version used 250 us.

**Connection record.** 48 bytes: generation, state, flags, ring, slot, last
activity, send chunk, parked receive buffer (id/offset/length), and a pointer
to a heap copy that only exists while a request is incomplete. Generations tag
every SQE so a completion for a slot that was closed and re-used is ignored.

**Send buffers.** A bounded pool of 512 x 72 KiB chunks (36 MiB, prefaulted;
a few dozen are in flight at 35k req/s);
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
are closed by a 10 s sweep; idle keep-alives are never timed out (the Nginx
contract asks for 65 s minimum, and the direct k6 users idle up to 15 s plus
latency).

**Dead peers.** `SO_KEEPALIVE` with a 120 s idle / 30 s interval / 4 probes is
set on the listener and inherited, so a peer that vanished without a FIN frees
its slot after about four minutes instead of never.

**Closing.** `shutdown(SHUT_RDWR)` first so the armed multishot receive
terminates, then `close_direct` from its final completion. A send still in
flight keeps its chunk until it completes; the generation check frees it.

**Memory guard** (rule 8: run out of memory and you lose). Every 100 ms the
loop reads `MemAvailable` and, when the process runs under a cgroup with
`memory.max`, the cgroup headroom too. Below 256 MB accepting pauses (the
multishot accept is cancelled; clients wait in the backlog); below 128 MB up
to 2,000 idle keep-alives per tick are closed, oldest first. Accepting resumes
32 MB above the pause line. The scored version used 192/96 MB at 250 ms; the
organizer's notes on other entries show `MemAvailable` falling by ~80 MB in
four seconds near the wall, and our 240k+ probes were refused connections.

**Prefault.** The connection table, send pool and receive pools are touched at
startup and the SQLite page cache is warmed with the feed and the 2,000 newest
posts, so the first thousands of requests do not take page faults.

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
- `cache_size=-32768` (32 MiB), `temp_store=MEMORY`, `wal_autocheckpoint=0`,
  `journal_size_limit=64 MiB`.
- Compile: `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DQS=0`, `OMIT_DEPRECATED`,
  `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_PROGRESS_CALLBACK`,
  `OMIT_DECLTYPE`, `OMIT_AUTOINIT`, `USE_ALLOCA`, `LIKE_DOESNT_MATCH_BLOBS`,
  `MAX_EXPR_DEPTH=0`, `-O3 -march=native -flto`. PGO (`make pgo`, trained on
  a synthetic database) is implemented but not shipped: it measured no CPU
  gain and lower throughput here (RESULTS.md).

  Why not `THREADSAFE=0`: the checkpoint thread needs a second connection, and
  with all mutexes compiled out the per-inode lock bookkeeping that both
  connections share (`unixInodeInfo`, the heap WAL index locks) is unprotected.
  `THREADSAFE=2` keeps the core mutexes (uncontended, a few per transaction)
  and drops the per-connection mutex, which is the expensive one.

**Feed as three range scans.** The 20 newest `(created_at, id)` come from the
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
commit) exceeds 4,000 frames not yet backfilled, so the loop never waits for an `fsync`. PASSIVE
checkpoints alone never let the WAL restart under continuous writes (the Zig
entry saw 5 MB/s of unbounded growth). So when the WAL reaches 16,384 frames
(64 MiB) the loop *holds writes* (reads continue; held requests park with their
input) until the checkpointer reports the WAL fully backfilled; the next write
transaction then restarts the WAL from frame 1. A hold gives up after 500 ms
and retries 16,384 frames later. (8,192 in the scored version; a hold lasts as
long as the checkpoint's fsyncs, not its copying, so a larger WAL means half
the holds.) Holds are counted and timed in the stats line.

**Create** uses `INSERT` then `SELECT created_at` by rowid (the column default
`strftime` produces the timestamp, as the SPEC describes). **Like** checks the
post exists, then `INSERT ... ON CONFLICT DO NOTHING`; `sqlite3_changes()`
decides 201 or 200.

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
- No direct B-tree page reader for the feed (rule 2 question; listed in
  STATUS.md).
- No assembly: the measured hot spots are the kernel TCP path and SQLite's VDBE,
  which C expresses as well as asm, and the exploration found hand-written AVX2
  no faster than intrinsics for JSON.
- No `Connection: close` hybrid above the table size; clients wait in the
  backlog instead.
- No SQPOLL, busy polling, zero-copy send or send bundles (negative or
  unavailable on 1 vCPU / kernel 6.8).
