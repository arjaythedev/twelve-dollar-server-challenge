# go-iouring-davidefrageri

**Language:** Go 1.27.3 (the standard library only) with the SQLite 3.54.1 amalgamation compiled in
through cgo, and a ~1,200-line C shim around it. **Served directly** (no Nginx) on `0.0.0.0:80`.

One OS thread does everything: io_uring for every socket operation, one SQLite transaction per
event-loop pass, and the HTTP responses of database requests rendered in C straight from SQLite's
own B-tree pages into the output buffers. There is no cache of anything across requests; every
request reads the database and verifies its token.

## How it works

### Transport (one thread, io_uring)

- Rings with `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL`, a 65,536-entry completion queue and a
  sparse **direct-descriptor table** sized to the hard `RLIMIT_NOFILE` (the soft limit is raised to
  it). Accepted sockets never become process file descriptors. When a ring's table is within a
  margin of full, accepting moves to the next ring (up to 16 rings) and the full ring's accepts are
  cancelled (left armed they end in `ENFILE`, each dropping one accepted connection).
- **8 `SO_REUSEPORT` listeners**, each with a **multishot accept** that allocates direct descriptors.
- **Multishot receive into a provided-buffer ring** (1,024 × 2 KB per ring): an idle connection holds
  no receive memory. When a burst empties the buffer ring, the connections that lost their receive
  go to a FIFO and are read with bounded `MSG_DONTWAIT` single-shot reads (never more in flight than
  there are buffers), then re-armed.
- **Drain first, answer bounded**: a pass reads up to 16,384 sockets but produces at most 2,048
  responses; the rest of the bytes wait in 512-byte per-connection buffers and are answered oldest
  first in later passes. Socket memory therefore never piles up on the receive side, which is what
  turns a stall into a collapse near saturation.
- A pass waits on ring 0 for 256 completions (spread over the rings) or `WINDOW_US` (default
  4,000 µs), then polls the other rings and drains them all; the other rings' sends are submitted at
  the end of the pass. Every poll is non-blocking and asks for many completions with a zero timeout
  (kernel 6.8 runs at most `max(min_complete, 20)` deferred task-work items per enter).
- One send in flight per connection, `shutdown`+`close` as a hard-linked pair, generation numbers in
  `user_data` so stale completions are ignored. Idle connections are never closed.

### Go runtime kept out of the hot path

`GOMAXPROCS=1`, `LockOSThread`, `GOGC=off` with a memory-limit backstop, `asyncpreemptoff=1`, raw
syscalls for io_uring (no sysmon hand-offs around the 1 ms wait), connection tables without
pointers, all buffers in `mmap`'d memory (their addresses are handed to C), `-gcflags=all=-B`,
`GOAMD64=v4` when the build CPU has AVX-512 (`v3` with AVX2, else the baseline).

### HTTP, JWT, JSON input (Go)

- Framing finds the end of the head with the standard library's SIMD `bytes.Index`, then looks only
  at the request line, `Authorization`, `Content-Length`, `Connection` and `Transfer-Encoding`
  (chunked → 400 and close). Heads over 16 KB and bodies over 1 MB are rejected.
- HS256 is verified on every request: signature first (canonical base64url, exactly 32 bytes),
  HMAC from two saved SHA-256 states (ipad/opad blocks hashed once at startup), constant-time
  compare, then `alg == HS256`, `exp` required and in the future, `sub` a positive integer string,
  `username` a string.
- Request bodies are fully validated JSON (what `JSON.parse` accepts); the last duplicate key wins,
  `body` is trimmed with JavaScript's `trim()` set and measured in code points.

### SQLite (C shim, `shim.c`)

- **One cgo call per pass.** Go collects the pass's database requests (kind, ids, user, body, name,
  a reserved output region per request) in one array; `td_batch` runs `BEGIN`, executes them in
  order, writes each complete HTTP response (status line, headers, JSON) into its region, and
  `COMMIT`s. Responses are handed to the kernel only after the commit; if the commit fails, every
  response of the batch becomes a 500 and its connection is closed (rule 6).
- **Reads walk SQLite's own B-tree pages through the pager**, inside the pass's read transaction:
  `getAndInitPage`/`releasePage`, the same `MemPage` and cell layout SQLite's cursors read. No cursor
  or pager state is written. The feed: the first 20 entries of `posts_created_at_id_idx` (a
  DESC,DESC index, so the newest posts are its first entries), then the 20 posts and their 20
  authors with a **run-based multi-key descent** (the idea of c-iouring-pavlyhalim #245, reworked):
  the keys are sorted, so on each level the keys that share a page form a run; an interior page is
  searched only for a run's first and last key and the whole run follows when both land on the
  same child; each child's key range is known from its parent's separators, so on a leaf with dense
  rowids the cell index is `key - first key`, verified by one probe; pages shared by a run are
  fetched and referenced once. The like counts of all 20 posts come from one range walk of
  `likes_post_id_idx` whose leaves are counted in a tight loop. One post is the same with one key.
  Any surprise (overflow payload, an id the table lacks, a post without a user, an unexpected key
  type) falls back to the reference SQL statement, prepared once.
- **Writes** are prepared SQL: `INSERT INTO posts … RETURNING id, created_at` and
  `INSERT INTO likes … ON CONFLICT DO NOTHING` + `sqlite3_changes()`, after a B-tree existence check
  for the 404.
- **Page cache plugged in through `SQLITE_CONFIG_PCACHE2`** (public API): an array indexed by page
  number instead of pcache1's hash chains and LRU list, page buffers in one anonymous mapping with
  `MADV_HUGEPAGE` (2 MB TLB entries), CLOCK eviction only when the budget (`TD_CACHE_MB`, default 128)
  is full. `xTruncate` at every commit scans the direct index between the limit and the largest page
  number stored, with the lock-byte page tracked apart, so it costs nothing in the common case
  (pcache1 scans its whole hash table once a B-tree split has parked a page at the lock-byte page;
  #245 and #343 work around that with a wrapper). `TD_PCACHE=default` restores pcache1.
- **WAL writes coalesced by a VFS shim** over `unix-excl` (public VFS API, the pattern of #245): the
  frames of a commit are written with one `pwrite` instead of two per frame; everything else goes
  straight through in order, and the commit frame is in the kernel before `COMMIT` returns.
  `unix-excl` keeps the wal-index in heap memory (no `-shm`, no `fcntl` per transaction).
  `TD_WALSHIM=0` disables it.
- `journal_mode=WAL`, `synchronous=NORMAL`, `wal_autocheckpoint=0`, `mmap_size=0`. Checkpoints run on
  a second connection in a thread at `nice 5`: PASSIVE past 4,096 frames (repeated while
  more than 128 frames remain), then the thread **syncs the database file itself and copies once
  more**, and only then are writes held between passes while it copies the last few dozen frames so
  the log restarts (the scheme of rust-iouring-kensac #264, with two changes learned the hard way;
  see "The first benchmark" below). The database file grows in 64 MB steps
  (`SQLITE_FCNTL_CHUNK_SIZE`, one `posix_fallocate` per step). Past 65,536 frames the loop
  checkpoints inline as a last resort.
- SQLite build: `THREADSAFE=2` + `OPEN_NOMUTEX`, `DEFAULT_MEMSTATUS=0`, `USE_ALLOCA`, the `OMIT_*`
  options of #264, `-O3 -march=native -mprefer-vector-width=256 -fno-plt -fno-stack-protector
  -fcf-protection=none`. JSON strings are checked for characters to escape 32 bytes at a time (AVX2)
  while being copied.

### Responses

Status line, `Content-Type: application/json`, `Content-Length` (a fixed 5-character field,
right-aligned with spaces, RFC 9110 §5.6.3 optional whitespace) and the body. Exact bytes per
SPEC.md; only `"`, `\` and control characters are escaped.

## Rules, point by point

- **Rule 5 (no caching across requests):** nothing is kept between requests except SQLite's page
  cache (the plugged-in cache holds exactly what SQLite asks it to hold, for as long as SQLite
  would), prepared statements and the two SHA-256 states of the HMAC key. Every request re-reads the
  pages it needs and re-verifies its token.
- **Rule 6 (durability):** WAL with `synchronous=NORMAL`; a pass's writes are one transaction and
  no response leaves before its `COMMIT` returned. The WAL shim changes only how many system calls
  carry a commit's frames, not what reaches the kernel or in which order.
- **Rulings of 2026-10-31 / 11-04:** reads use SQLite's pager and page structures without writing
  any cursor state; the page cache and the VFS are the documented plugin APIs; the amalgamation is
  unmodified and fetched by `build.sh` with its sha256 checked.
- **Rule 11:** `install.sh` installs a compiler and the Go toolchain only; no sysctl, no limits, no
  services. The direct-descriptor table is sized from the `LimitNOFILE` the organiser sets.

## Knobs (environment, all optional)

| variable | default | meaning |
|---|---|---|
| `WINDOW_US` | 4000 | how long a quiet pass waits for completions |
| `ANSWER_ORDER` | fifo | `lifo` answers the backlog newest first |
| `TD_CACHE_MB` | 128 | page cache budget (huge-page backed) |
| `TD_PCACHE` | | `default` uses SQLite's pcache1 instead |
| `TD_WALSHIM` | 1 | `0` writes WAL frames the way SQLite does |
| `TD_CK_NICE` | 5 | priority of the checkpoint thread |
| `TD_CHUNK_MB` | 64 | database file growth step (`0` disables) |
| `LOG_S` | 60 | seconds between stats lines on stderr |

## Measurements

**Correctness.** `test/run.sh`: 42/42 on a fresh seed copy (byte-exact against the golden files).
Beyond the suite: pipelined requests (answered in order, one database request per connection per
pass), requests split across writes, `Connection: close`, HTTP/1.0, a 20 KB head and chunked
bodies (400 and close), a 66 s idle keep-alive, duplicate keys and escapes in JSON bodies, 500/501
code points. Durability: 8 writers posting as fast as they can, `kill -9` after 2 s, every one of
the 10,563 acknowledged posts present afterwards, `PRAGMA integrity_check` ok, restart on the same
files fine. The stats line (stderr, every `LOG_S` seconds) shows `fallback=0` under load: every
read takes the B-tree plan.

**CPU per request (relative, development VM).** Measured before the benchmark box was available,
on an arm64 Linux VM with the load generator on the same machine (a Go port of `bench/load.js`
with think times scaled down 10×, 10,000 users, ~10,500 requests/s, about 10 requests per 1 ms
pass), server CPU from `/proc/<pid>/stat`. Interleaved rounds against the two reference entries
built in the same VM:

| entry | user µs/req | total µs/req |
|---|---:|---:|
| this | 2.58 – 2.87 | 7.7 – 8.7 |
| go-iouring-trungdlp #343 | 3.03 – 3.44 | 8.1 – 10.2 |
| rust-iouring-kensac #264 | 2.71 – 4.14 | 7.7 – 11.2 |

The VM's run-to-run noise is of the same size as the differences, so this only says the
user-space cost is in the right range; the box decides. The user-space profile (`perf`, cpu-clock)
is dominated by the author lookups (`seek_many` + the in-page search, ~28 %: 20 random rows of
`users` per feed, memory-bound), then JSON escaping/rendering (~10 %), record decoding (~5 %) and
SQLite's pager layer (~6 %); the Go side (framing, JWT, io_uring bookkeeping) is under 10 %.

**On the benchmark box** (Synteq 1 vCPU, Xeon 8260, kernel 6.8.0-117; the load generator runs on
the same vCPU, so these are relative numbers; `WINDOW_US=1000`, interleaved rounds, fresh seed copy
each run):

| 8,000 users, ~8,400 req/s | user µs/req | sys µs/req | total µs/req |
|---|---:|---:|---:|
| this | 7.39 / 7.50 | 12.18 / 11.84 | 19.57 / 19.34 |
| rust-iouring-kensac #264 | 8.25 / 8.45 | 12.24 / 12.24 | 20.50 / 20.69 |
| go-iouring-trungdlp #343 | 8.37 / 8.37 | 11.37 / 11.80 | 19.73 / 20.17 |

| 12,000 users, ~12,650 req/s | user | sys | total |
|---|---:|---:|---:|
| this | 6.19 / 6.13 | 10.91 / 10.65 | 17.10 / 16.78 |
| rust-iouring-kensac #264 | 6.95 / 7.03 | 10.11 / 10.06 | 17.06 / 17.09 |

Microbenchmarks of the shim on the box (`perf stat`, hardware counters), per feed request:
the 20 author lookups 3.1 µs with the run-based descent against 6.6 µs with 20
`sqlite3BtreeTableMoveto` calls on the same pages; the whole feed plan plus rendering 6.2 µs
(it started at 11.4 µs with the per-key prefetched descent of the first version; the win came
from doing less bookkeeping per key, not from prefetching).

What did not show: SQLite's pcache1 (`TD_PCACHE=default`) is as fast as the direct-indexed cache
on this box, and the WAL shim saves 3.5× the `pwrite` calls but no measurable CPU (writes are 8 %
of requests). Both stay in and on, switchable, because they cost nothing and are allowed. What did
show: the pass window. A longer quiet-pass wait amortizes the per-pass work (the enter and wake-up,
the transaction, the batch call) over more requests, for a few milliseconds of latency against a
500 ms budget. The default is therefore 4,000 µs (`WINDOW_US`):

| window | 8,000 users: total µs/req (user / sys) | p50 / p99 | 12,000 users: total (user / sys) | p50 / p99 |
|---|---:|---:|---:|---:|
| 1 ms | 18.88 (7.47 / 11.42) | 1.1 / 5.2 ms | 17.04 (6.19 / 10.85) | 1.1 / 52 ms |
| 2 ms | 16.36 (6.44 / 9.92) | 1.3 / 5.0 ms | 15.56 (5.37 / 10.19) | 2.2 / 45 ms |
| 4 ms | 15.70 (5.71 / 9.99) | 3.4 / 8.3 ms | 14.84 (4.22 / 10.61) | 4.5 / 95 ms |
| 8 ms | 15.09 (3.97 / 11.12) | 7.1 / 15 ms | 14.73 (4.24 / 10.49) | 8.5 / 82 ms |

(p99 at 12,000 users is dominated by the load generator sharing the vCPU.) `ANSWER_ORDER=lifo`
made no measurable difference at these loads and stays off.

### The first benchmark, and the checkpoint fix

The organiser's run of the first version (commit `07004d1`) held **250,000 users** (24,053 req/s,
p95 94.6 ms, p99 271.7 ms, 0 errors, 78 % app CPU, 30 µs per request); 260,000 and 280,000 failed
on p99 (1.4 s and 2.5 s) with the CPU at 72-78 %, i.e. not on CPU. The cause was the checkpoint
thread: at `nice 19` it was starved exactly when the loop was saturated, so the write hold ran to
its 250 ms cap without the log restarting (the 272 ms p99), the WAL grew past 65,536 frames and the
inline checkpoint stalled every request for hundreds of milliseconds (the cliff).

Reproduced on the box with the load generator on the same vCPU (60,000 users, think times scaled to
~18,000 req/s, 150 s hold): holds averaged **173 ms** even at normal priority. `strace -T` on the
thread showed why: a passive checkpoint that does not complete the backfill writes its pages to the
database file without syncing it, so the held tail's `fdatasync` flushed the whole cycle (~14 MB,
117-167 ms on this disk), and when the file had grown by a few pages the sync also waited for the
file system's journal. With the thread syncing the database before the hold, one more passive copy,
and a 64 MB chunk size: holds **17 ms average, 68 ms max**, the log restarts every cycle
(75 of 75), and over 2.96M requests the hold-period latency was p50 5.7 ms, **p95 10.4 ms,
p99 56.5 ms**, 0 errors, 17.0 µs CPU per request (generator included on the vCPU). The stats line
now reports every checkpoint cycle (`hold_ms`, `tail_ms`, `restarts`, `inline`).

### The second benchmark: four rings

The organiser's run of the checkpoint fix (commit `6de54ca`) held **230,000 users** (22,348 req/s,
p95 31.1 ms, p99 128.8 ms, 0 errors, 69 % CPU, 28 µs per request); 240,000 failed on p99 (6.9 s at
81 %). Lower than the first version, with a better p95 and less CPU per request. Two things differed
from every local test, which had run on one ring (at most 60,000 connections): on the box, with
`LimitNOFILE=65535`, 230,000 connections live on four rings; and the 64 MB chunk size was not what
it claimed to be.

Reproduced at 60,000 users by lowering the service's descriptor limit to 16,384 (four rings): the
same CPU per request, but the median doubled (5.8 → 11.4 ms), p95 doubled (11.1 → 22.5 ms), the
longest hold went from 52 to 172 ms, and every ring spill reset 8 connections. Causes and fixes:

- The loop polled rings 1-3 *before* the ring-0 wait. Their deferred task work runs only when they
  are entered, so what arrived on them during the wait was drained a pass late, and their sends,
  queued during the pass, were only submitted at that next poll. Now: wait on ring 0, poll the
  others, drain, and submit the other rings' sends at the end of the pass. The completion count that
  cuts a pass short is spread over the rings.
- A full ring's multishot accepts stayed armed and ended in `ENFILE`, each dropping one accepted
  connection; they are cancelled when accepting moves on.
- `HAVE_POSIX_FALLOCATE` was not defined, so SQLite "allocated" each 64 MB step by writing one byte
  into every 4 KB block: 16,384 writes, 64 MB of dirty pages and a second-long sync in the
  checkpoint thread, about a minute into every run (the seed is 249 MB; the file crosses 256 MB
  after ~7 MB of growth). Now one `posix_fallocate`.
- The checkpoint thread runs at `nice 5` instead of 0. At nice 0 it took half the CPU from the loop
  whenever both were runnable, so every cycle's burst put the loop behind by the burst's length; at
  nice 5 it runs in the loop's idle time and still gets a quarter of a contended CPU (the nice-19
  starvation of the first version came from 1.5 %).

60,000 users, ~18,000 req/s, 150 s hold, generator on the same vCPU, hold-period latency:

| | one ring, `6de54ca` | four rings, `6de54ca` | four rings, this | one ring, this |
|---|---:|---:|---:|---:|
| p50 / p95 / p99 | 5.8 / 11.1 / 39.5 ms | 11.4 / 22.5 / 49.9 ms | 5.7 / 11.8 / 39.2 ms | 5.7 / 9.9 / 29.8 ms |
| longest hold | 52 ms | 172 ms | 35 ms | 70 ms |
| errors | 0 | 24 (resets at spills) | 0 | 0 |
| CPU per request | 16.72 µs | 17.02 µs | 16.81 µs | 16.50 µs |

**Not measured:** a run at 300,000+ users. That needs a load generator in the same network with
several source addresses (one address holds at most ~64k connections to one port), which was not
available. The multi-ring spill has been exercised at 16,384 slots per ring (three spills, no
errors), not at 65,535. The CPU per request at 230,000 (28 µs against #264's 25 µs at 310,000) is
not explained by any of the above and could not be profiled at that scale.

## Credits

Ideas taken, with thanks, from rust-iouring-kensac (#264: the pass structure, group commit,
checkpoint hold, drain-first policy), c-iouring-pavlyhalim (#245: multi-key descent, interpolated
in-page search, the WAL coalescing shim, the `xTruncate` observation) and go-iouring-trungdlp
(#343: the Go io_uring binding shape and the Ubuntu 6.8 provided-buffer-ring quirk). All code here
is written from scratch.
