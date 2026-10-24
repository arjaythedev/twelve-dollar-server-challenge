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

Ring 0 is where the thread blocks (`io_uring_submit_and_wait_timeout`). The
other rings' fds are watched from ring 0 with multishot `POLL_ADD`; when one
fires, that ring gets `io_uring_get_events` and its CQ is drained. All rings use
`SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | SUBMIT_ALL`, with a fallback
chain to plain flags on older kernels. Polling a `DEFER_TASKRUN` ring's fd was
verified to work on this kernel (6.3+ has the lazy `poll_wq`); the poll must be
re-armed whenever a CQE arrives without `IORING_CQE_F_MORE`, which happens.

**Accept.** One multishot accept (`IORING_FILE_INDEX_ALLOC`) is armed at a time,
on the ring with the most free slots. When that ring is within 64 slots of
full the accept is cancelled and re-armed elsewhere. `-ENFILE` (table full)
terminates the multishot; it is re-armed when a close frees a slot. The listen
backlog (4,096, the kernel default `somaxconn`) absorbs the gap, so clients wait
rather than get refused.

**Receive.** One multishot `RECV` per connection with `IOSQE_BUFFER_SELECT`
from a pool of provided buffers per ring (2,048 x 2 KiB). An idle connection
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
32 CQEs or 250 us, whichever first; otherwise it waits for 1 CQE (50 ms
timeout for the periodic tasks). A 32/250 us batch is what the NASM and Zig
entries converged on; 128/4 ms starved low connection counts.

**Connection record.** 48 bytes: generation, state, flags, ring, slot, last
activity, send chunk, parked receive buffer (id/offset/length), and a pointer
to a heap copy that only exists while a request is incomplete. Generations tag
every SQE so a completion for a slot that was closed and re-used is ignored.

**Send buffers.** A bounded pool of 2,048 x 16 KiB chunks (32 MiB, prefaulted).
A response is written straight into a chunk, the head is placed right before
the body, and the chunk is returned on the send completion (short sends resend
the remainder). When the pool is empty the connection parks with its input
until a chunk frees up; nothing is dropped.

**Pipelining and partial requests.** One request per connection per batch;
remaining bytes stay parked until the send completes, then the connection is
re-queued. Incomplete requests move into a per-connection heap buffer (24 KiB
cap: 8 KiB head + 16 KiB body) so the shared receive buffer is not held. Headers
over 8 KiB answer 413 and close; bodies over 16 KiB too. `Expect: 100-continue`
gets its interim response. Connections that leave a request incomplete for 30 s
are closed by a 10 s sweep; idle keep-alives are never timed out (the Nginx
contract asks for 65 s minimum, and the direct k6 users idle up to 15 s plus
latency).

**Closing.** `shutdown(SHUT_RDWR)` first so the armed multishot receive
terminates, then `close_direct` from its final completion. A send still in
flight keeps its chunk until it completes; the generation check frees it.

**Memory guard** (rule 8: run out of memory and you lose). Every 250 ms the
loop reads `MemAvailable` and, when the process runs under a cgroup with
`memory.max`, the cgroup headroom too. Below 192 MB accepting pauses (the
multishot accept is cancelled; clients wait in the backlog); below 96 MB up to
2,000 idle keep-alives per tick are closed, oldest first. Accepting resumes 32 MB
above the pause line.

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
`likes_post_id_idx` grouped by `post_id` and one range scan of `posts JOIN
users` over the span replace 20 + 20 + 20 point lookups. Rows are copied into
a scratch area and emitted in the index order. Otherwise, or on any anomaly,
the reference query runs. `--feed-reference` forces the reference query; the
black-box suite checks both strategies produce identical feeds. Every value is
read on the request that serves it (rule 5).

**Checkpoints off the loop.** A second connection on a `SCHED_IDLE` thread runs
`PASSIVE` checkpoints whenever the WAL (reported by the WAL hook after every
commit) exceeds 4,000 frames not yet backfilled, so the loop never waits for an `fsync`. PASSIVE
checkpoints alone never let the WAL restart under continuous writes (the Zig
entry saw 5 MB/s of unbounded growth). So when the WAL reaches 8,192 frames
(32 MiB) the loop *holds writes* (reads continue; held requests park with their
input) until the checkpointer reports the WAL fully backfilled; the next write
transaction then restarts the WAL from frame 1. A hold gives up after 500 ms
and retries 8,192 frames later. Holds are counted and timed in the stats line.

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
- Body rules: trim ASCII whitespace, then count UTF-8 code points: empty is
  "body is required", more than 500 is "body must be at most 500 characters".

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
