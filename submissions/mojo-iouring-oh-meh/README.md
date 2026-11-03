# Mojo + io_uring + SQLite

One thread of Mojo on io_uring, with SQLite compiled into a small C shim. Direct on port 80.

| | |
|---|---|
| Language | Mojo 1.2.0 (pinned PyPI wheel, 40 MB, installed into `/opt/mojo` by `install.sh`), `mojo build` at -O3 |
| Framework | none: hand-written HTTP/1.1, JSON, JWT and SHA-256 in Mojo |
| io_uring | liburing 2.15, built from the pinned tarball; needs Linux 6.1+ |
| SQLite | 3.54.1 amalgamation, SHA-256 pinned, compiled into `src/shim.c` with GCC 13 (`-O3 -march=native`), not patched |
| C | `src/shim.c`, ~600 lines: wrappers for liburing's inline prep helpers, SQLite's file-private B-tree calls, a checkpoint thread, the code lock. No request logic |
| **Nginx or direct** | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box) |

I picked Mojo because nobody had, and because the language question is already settled by
the entries above this one: the top of the board is the same design in C, Zig, Rust,
Crystal, Ada and assembly. What was left to find out is whether a three-month-old language
can hold that design with only plumbing in C. The hot path (parsing, auth, JSON, record
decoding, the query plans, the event loop) is Mojo.

## Running it

```bash
sudo bash install.sh   # build-essential, python3, Mojo 1.2.0 into /opt/mojo (about 40 s)
bash build.sh          # fetches + verifies SQLite and liburing, builds bin/server (about a minute)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` runs in the foreground and reads only the SPEC.md variables (plus
`MOJO_TABLE_OVERRIDE`, a test-only knob for forcing tiny descriptor tables; never set on the box).

## Design

- **One thread.** The event loop, HTTP, JWT, JSON and SQLite reads all run on one thread. A second
  C thread only runs passive WAL checkpoints, at nice 19. SQLite is built `THREADSAFE=2` for that.
- **Rings.** Accepted sockets are io_uring direct descriptors, so `LimitNOFILE=65535` caps one
  ring's table, not the server. Rings open lazily, each with `RLIMIT_NOFILE - 64` slots, up to 16.
  Flags: `SINGLE_ISSUER`, `DEFER_TASKRUN`, `TASKRUN_FLAG`, `CQSIZE`, `SUBMIT_ALL`.
- **Accept** is one-shot direct accepts (`IORING_FILE_INDEX_ALLOC`), up to 64 in flight and never
  more than the current ring has free slots; accepting moves to the next ring when one fills. I
  started with multishot accept and it lost one connection per ring: a burst overruns the table and
  the overflow is accepted then dropped with `ENFILE`.
- **Receive** is multishot into provided buffers, 2,048 × 2 KB per ring. Requests are parsed in
  place and the buffer goes back at the end of the batch, in contiguous runs with the completion
  skipped. Idle connections hold no buffer. A connection record is 64 bytes in a `calloc`ed table.
- **Send** is one `SEND` per response from an arena the batch writes into; the arena is reused once
  its last send completes. Pipelined requests are answered in order, one send in flight per
  connection. Constant error responses are built once at startup.
- **Sleeping.** With one ring live, each batch ends in a single `io_uring_enter` that submits and
  waits. With several, the thread sleeps in `epoll_wait` on the rings' eventfds (a `DEFER_TASKRUN`
  ring signals it when work arrives) and runs the task work of the rings that fired.
- **Batches form when busy.** After a pass that handled 8 or more completions, the next wait asks
  for 32 completions or 250 µs (one ring: `io_uring_submit_and_wait_timeout`; several: a 250 µs
  pause after epoll, then every ring's task work), so one wake-up and its `BEGIN`/`COMMIT` and
  submits are shared by more requests. It delays only when responses leave, never what they
  contain, and only while the server is busy. Locally it cut CPU per request by about 15 %.
- **Batches.** Everything from one wake-up is one batch: reads are answered from one `BEGIN`, with
  B-tree cursors opened once per batch; then cursors close, writes run as prepared statements, then
  `COMMIT`, then the sends are submitted (read responses can go out earlier if the submission queue
  fills mid-batch; they are built from committed state either way). A write's response is only built
  after the commit, and it holds its place in the connection's response queue from the moment it is
  parsed, so a pipelined write and read are answered in request order. A failed commit turns the
  batch's writes into 500s.
- **Closing.** Healthy keep-alive connections are never closed by the server. When it does close
  (HTTP/1.0, `Connection: close`, a bad request, or after the peer's EOF once queued responses are
  out), it is a `SHUTDOWN` hard-linked to the direct-descriptor `CLOSE`, so the outstanding
  multishot receive completes instead of keeping the socket alive. A guard pauses accepting when
  `MemAvailable` drops under 160 MB and resumes above 256 MB.

## SQLite

- Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `cache_size=-32768` (32 MiB), `mmap_size=0`,
  `temp_store=MEMORY`, `wal_autocheckpoint=0`. VFS `unix-excl`, so the WAL index is in heap memory
  and a transaction takes no fcntl lock.
- **Reads walk the B-tree.** `GET /feed` is the last 20 rows of the posts tree by rowid, one range
  scan of `likes_post_id_idx` for all 20 like counts, and a rowid seek into `users` per row.
  `GET /posts/:id` is a rowid seek plus the same two lookups. The calls are SQLite's own
  (`sqlite3BtreeTableMoveto`, `IndexMoveto`, `Next`, `Previous`, `PayloadFetch`), through its
  pager, WAL and locking, inside the batch's transaction; only the bytecode engine is skipped, as
  the organizer allowed for the Lean entry. Record decoding is `src/record.mojo`.
- The feed plan is only right when post ids are dense and `created_at` never decreases with id
  (then `ORDER BY created_at DESC, id DESC` is reverse rowid order). The server checks both with
  SQL at startup, and after every insert by reading the previous row back from SQLite and
  comparing it with the `RETURNING` values. The only thing kept between requests is one boolean
  saying which plan to use; it flips to the reference SQL query for good if a check ever fails.
  No data is remembered across requests.
- **Writes** are the reference `INSERT ... RETURNING` and `INSERT ... ON CONFLICT DO NOTHING`,
  group-committed per batch.
- **No routine checkpoint work on the loop.** The checkpoint thread runs `PASSIVE` every 250 ms on
  its own connection (which runs one read first: a connection that has never run a statement has
  not opened the WAL, and a checkpoint on it is a no-op; the thread logs its first real checkpoint
  at startup), at nice 19, so on the one vCPU the loop is scheduled ahead of it. SQLite only
  restarts a WAL when a write begins with every frame already in the database file, and group
  commit never leaves such a moment, so the loop makes one: past 8,192 frames (32 MiB, two
  wal-index hash segments) it holds its writes (reads go on) while the thread runs passes back to
  back, and once the log is fully backfilled the next batch's first frame restarts it at frame 1
  inside SQLite's own frame write. A hold ends when the thread reports the log copied, or after
  1 s; while it lasts the loop polls every 2 ms, so the end is seen even with no traffic. A write
  caught in a hold waits for it, and so does anything pipelined behind that write on its
  connection; a hold is one pass plus the thread's wake-up, 20-50 ms on an idle disk and
  50-460 ms with the box's memory pinned to the benchmark's level. What stays on the loop is
  SQLite's sync of the new WAL header at each restart (`synchronous=NORMAL` syncs the header
  when a log is reused): one fsync per 8,192 frames, where the earlier version's `TRUNCATE`
  checkpoint on the loop cost two fsyncs plus the copy. That `TRUNCATE` remains as the last
  resort past 16,384 frames, and the log says once a minute how many holds, restarts, expired
  holds and loop truncates there were (the last two should stay 0). `MOJO_WAL_HOLD_OVERRIDE`
  is a test-only knob that lowers the threshold so the probes can exercise a hold.
  Measured on a 1 vCPU / 2 GB droplet with 1.1 GB pinned and 120,000 users of the benchmark's
  model, 3-minute holds: `ef45beb` ran 69 loop-thread `TRUNCATE`s (51 ms average, 215 ms worst);
  this version 62 holds, 62 restarts, 0 expired, 0 truncates; the reads' p99 over 10-second
  windows went from 54 ms (median window) and 314 ms (worst window) to 11 ms and 72 ms, p99 over
  all requests from 161 to 35 ms, and the writes' p99 from 55 ms (median window) and 400 ms (worst)
  to 28-37 ms and 319 ms: a write caught in a hold waits for the pass, the rest do not.
- **The binary's code and read-only data are locked in RAM** (`mlock` of each private read-only
  file-backed mapping, the binary's own 1.6 MB first, then the shared libraries until systemd's
  8 MB `RLIMIT_MEMLOCK` is used up; 8,156 KB in all on the droplet). With no swap, reclaim near
  the memory wall takes file-backed pages first, and the loop then stalls on major faults reading
  its own code back from disk: 21 of them on the loop thread in the 3-minute hold above, 2 with
  the lock. The log says how much was locked and whether the binary itself was locked in full.
- `mmap_size=0` on purpose: with mmap on, SQLite skips its page cache for mapped pages and consults
  the WAL index on every page touch, which was 12 % of CPU. With a 32 MiB page cache the hot set
  (users, the tails of posts and of the likes index) is a cache hit.
- The startup invariant scan runs on a throwaway connection with no mmap and a tiny cache, and the
  file's pages are dropped from the page cache afterwards, so the server starts at about 10 MB
  rather than with the whole posts table charged to it.

## Auth and HTTP

- HS256 only; `exp` required and checked against the batch's clock; `sub` must be a positive
  integer string; `username` is used as-is. HMAC-SHA256 is in `src/sha256.mojo` with the key's
  inner and outer pad states hashed once at startup; every token's signature is checked on every
  request. The self-test (`src/selftest.mojo`) checks SHA-256 and HMAC against the RFC 4231 vector
  and a token signed the way `test/test.sh` signs one.
- The parser reads the request line, `Authorization`, `Content-Length` and `Connection`; everything
  else is skipped. Requests that span receives are completed in a per-connection buffer that grows
  by `realloc` from 512 bytes to a 16 KB cap (413 past it). HTTP/1.0 or `Connection: close` closes
  after the response.

## Measurements

No user-count score is claimed. Two machines, neither the organizer's.

**x86, the benchmark's shape:** a DigitalOcean 1 vCPU / 2 GB Premium Intel droplet (2.0 GHz,
1,967 MB, Ubuntu 24.04, kernel 6.8.0-147), started by systemd as `deploy` with `LimitNOFILE=65535`
and `CAP_NET_BIND_SERVICE` on `0.0.0.0:80`; k6 on a 4 vCPU droplet in the same datacenter over the
private network. That load box is the limit of what I could drive: about 16,000 req/s and 20,000
users.

| Run | Result |
|---|---|
| `install.sh` / seed / `test/run.sh` | 35 s / 31 s / 64 s (build plus 42/42 checks), 369 MB peak during the build |
| user model without think time, 500 connections, 30 s | 16,000 req/s with the server at 45 % CPU: **28 µs of CPU per request, kernel included**; server-side feed latency median 1.2 ms, p90 3.8 ms |
| `bench/load.js`, 20,000 users, 60 s ramp + 120 s | 0 errors; p99 4.4 ms as k6 saw it, 0.8 ms median from a separate probe; **app cgroup peak 132 MB**; kernel slab grew 83 MB, about 4.2 KB per idle connection |
| idle | 34 MB cgroup, 11 MB RSS |

perf during the saturation run: 65 % kernel (the virtio notify `iowrite16` alone is 10 %), 31 %
server, 3 % libc. On the server side the largest items are HTTP parsing,
`sqlite3BtreeTableMoveto` for the users rows, record decoding and `pcache1Fetch`.

If those numbers carried over to the benchmark box (same RAM, a faster Xeon), CPU would run out
around 35,000 req/s and memory around 380,000 connections. Those are extrapolations.

**arm64 VM** (4 vCPU Ubuntu 24.04 on a laptop, k6 over loopback), where it was developed:
42/42; 0 errors in `bench/load.js` at 2,000 and 10,000 users with p99 0.6 ms; 79,600 req/s and
5.2 µs of server CPU per request with no think time on one ring, 77,300 req/s and 5.6 µs forced
onto eight 64-slot rings.

## Honesty notes

- Ubuntu's 6.8 kernels reject `IORING_REGISTER_PBUF_RING` (EINVAL for any argument set; checked
  with strace), so this uses the older `IORING_OP_PROVIDE_BUFFERS` op instead.
- The multi-ring path was exercised with 64-slot tables, not by filling a 65,471-slot table with
  real connections. The table limit itself is the kernel's `RLIMIT_NOFILE` check in
  `io_sqe_files_register`.
- Nothing above 20,000 simultaneous connections has been run.
- Two rounds of independent review (a second model, read-only) found six lifecycle defects the
  suite cannot see, because loopback delivers every test request in one segment and the suite
  never pipelines: a use-after-realloc on fragmented requests, reversed responses when a write and
  a read were pipelined, a leaked response arena on EOF with a write queued, a close that could
  pre-empt a pipelined write's response, a freed queue node that a later write could fill, and a
  stale submission-queue entry when the close helper found only one free slot. Each has a probe
  (fragmented POST, mixed pipelines, half-close, pipelined `Connection: close`) that now passes.
- The Mojo runtime starts a few worker threads at startup. The server never gives them work.
- The B-tree calls are to SQLite's own layer and stay inside its transactions. The amalgamation is
  not modified.

## Layout

```
src/server.mojo   event loop, rings, connection table, batches, routing
src/db.mojo       transactions, feed/post plans on B-tree cursors, writes
src/http.mojo     request parser
src/jwt.mojo      base64url, HS256 verification
src/sha256.mojo   SHA-256 and HMAC
src/jsonscan.mojo JSON validation, the "body" field, JWT payload fields
src/record.mojo   SQLite record format
src/resp.mojo     response arenas and headers
src/bytes.mojo    raw byte helpers, the writer, JSON escaping
src/uring.mojo    typed wrappers over the shim
src/selftest.mojo unit checks for the leaf modules
src/shim.c        io_uring plumbing, SQLite B-tree exports, checkpoint thread
```
