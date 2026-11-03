# C + io_uring + SQLite

| | |
|---|---|
| Language | C (GCC 13.3, Ubuntu 24.04's `build-essential`), `-O3 -march=native -flto`, profile-guided, built on the box it runs on |
| Framework | none: one `io_uring` event loop, hand-written HTTP/1.1, JSON and JWT code |
| io_uring | liburing 2.12, built from the release tarball (SHA-256 pinned in `build.sh`) |
| SQLite | 3.54.1, the unmodified official amalgamation (SHA-256 pinned in `build.sh`), one translation unit with the server |
| Other dependencies | none (SHA-256/HMAC, base64url and JSON are in `server.c`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

Needs Linux 6.1 or newer with io_uring enabled (Ubuntu 24.04's 6.8 kernel has it on by default).

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip
bash build.sh          # downloads and verifies SQLite and liburing, builds bin/server (about 2 minutes)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- **One event-loop thread.** The box has one vCPU, so more loops would only add context switches. A
  second thread only runs checkpoints. Every ring is `DEFER_TASKRUN` and `SINGLE_ISSUER`, so a ring's
  completion work runs when the loop enters that ring and never interrupts a pass. Each pass gives
  each extra ring one non-blocking `io_uring_enter`, then waits on ring 0.
- **Wake-ups are batched when busy.** If the last pass handled 4 or more completions, the wait asks for
  ring 0's share of 128 completions (capped at a third of open connections, so a few clients without
  think time never sit out the timer) or 2 ms, whichever comes first; otherwise it wakes on the first
  one. With more than one ring, or while a checkpoint round is running, the wait is always timed, so
  other rings and held writes never depend on a new event on ring 0. Each wake-up then
  carries more requests, so the syscalls, the batch's `COMMIT` and the send submissions are shared by
  more of them. It only delays when responses are sent, never what they contain: every response still
  waits for its batch's commit.
- **Accepted sockets are registered (direct) descriptors, not process descriptors.** Accepts use
  `IORING_FILE_INDEX_ALLOC`, so a client occupies a slot in the ring's registered file table and the
  process descriptor table only holds the listener, the rings and SQLite's files. The kernel caps one
  table at `RLIMIT_NOFILE`, so when a table is full the loop adds another ring (same thread, up to 32)
  and accepts there. With `LimitNOFILE=65535` that is about 2 million clients in one process, instead
  of forking a new worker every 65,535.
- **Eight `SO_REUSEPORT` listeners, single-shot accepts, 64 in flight on each.** A listen queue is
  capped at `net.core.somaxconn` (4,096 on the box), so eight queues hold 32,000 pending connections
  through a ramp. A multishot accept can't be stopped before its table fills, and the accept that finds
  the table full has already taken the connection off the queue, so the kernel can only reset it
  (measured: the client at slot 1,025 got `ECONNRESET`). Single-shot accepts are only issued while a
  table has room. Each listener counts its own: connections are hashed to listeners, and with one
  shared count a listener could end up with none armed while accepts armed on idle listeners never
  completed (measured: a few hundred requests waited out the client's 60 s timeout after a ramp).
- **Multishot receives into provided buffer rings** (2,048 x 1 KB per ring, 2 MB rather than 8 MB: the ring cycles through all of them, so all of them stay resident). A receive stays armed for
  the life of the connection; each chunk arrives in a kernel-picked buffer that goes back to the ring as
  soon as it is parsed. Idle clients hold no buffer. A partial request is copied into a per-connection
  buffer until the rest arrives. If the ring ran dry (`ENOBUFS`) the receive is rearmed after the batch.
- **A transaction per completion batch (group commit).** The first request in a batch that touches the
  database opens the transaction; every read and write in the batch runs inside it, and the batch
  ends with one `COMMIT`. Responses are written into an arena and only handed to `send` SQEs after that
  `COMMIT` returns, so a `201` always means the row is committed (rule 6). If a commit fails, nothing
  from that batch is sent: those connections are closed instead.
- **Sends go through io_uring**, one in flight per connection so responses stay in order (later ones
  queue behind it). Each send owns a copy of its bytes, so the arena is reused immediately and a slow
  reader can never pin it. Short sends resubmit the rest. Closing a connection cancels its pending
  receive (and send, if one is in flight) and closes the slot in one linked chain; generation numbers in
  the completion tags make completions for a reused slot harmless. The cancels name the operations by
  their completion tags, which the kernel looks up in a hash table. Cancelling by descriptor instead
  walks every pending operation on the ring (one receive per open connection): with 20,000 idle
  clients that made each close cost about 300 us of CPU instead of about 20 us.
- **Reads use SQLite's B-tree cursors, not its SQL engine.** `server.c` includes the unmodified
  amalgamation. `/feed` walks `posts_created_at_id_idx` for the 20 newest, counts their likes in one walk
  of `likes_post_id_idx` (reading a leaf's cells directly), fetches the rows in ascending id order so
  consecutive ids are one step along a leaf, then looks up each author. `/posts/:id` is the same plan for
  one row. Records are decoded in place. Every request still reads its rows from SQLite while it is
  served (rule 5); what is kept between requests is prepared statements, cursor memory, `KeyInfo` objects
  and root page numbers.
- **SQL is the fallback.** At startup the server checks the columns and sort order of everything it
  decodes; on any difference every read uses the reference SQL. At run time any record it doesn't expect
  (overflow pages, a post without a user, a column of another type, a missing post) sends that request to
  SQL. Writes always use prepared SQL. A like's post check is a B-tree seek.
- **Checkpoints run on a second thread at nice 19**, on their own connection, through the `unix-excl` VFS:
  the process owns the file, the WAL index lives in its heap, and a transaction takes no file locks. After
  8,192 WAL frames (32 MB) the loop wakes the thread, which runs a `PASSIVE` checkpoint while the loop goes on. A
  log only restarts once fully copied back, which steady writes prevent, so when the bulk pass ends new
  POSTs are held unread for at most 250 ms (reads go on) while the thread copies the tail. Past 32,768
  frames (a CPU saturated long enough to starve the thread; a write-only run reached 1.6 GB of WAL in 40 s
  without this) writes stay held until the thread finishes, however long that takes, and if the thread is
  idle the loop checkpoints by itself. Durability is unchanged: WAL with
  `synchronous=NORMAL`, and a response waits for its `COMMIT`. If a commit fails, nothing from that batch
  is sent and its connections are closed.
- **Provided-buffer registration retries.** Ubuntu's 6.8.0-136 and later kernels reject a buffer ring whose
  reserved words are zero (Launchpad #2162843), which stops a server from starting at all. It retries once
  with `resv[0] = 1`, which correct kernels never see.
- **Profile-guided build.** `build.sh` first builds an instrumented binary with `train.c` compiled in.
  It creates a synthetic database (20k users, 200k posts, 600k likes; the schema's tables and indexes,
  none of the challenge seed) and serves 300,000 requests in the benchmark mix from memory: parsing,
  JWT checks, queries, writes and commits. The real binary is then compiled with that profile. The
  training binary and database are deleted; `train.c` is not in the final binary.
- **SQLite settings**: WAL, `synchronous=NORMAL`, no `mmap`, a 64 MB page cache (a cached page keeps its
  parsed header and needs no WAL lookup; `mmap` measured 4% more CPU per request), prepared statements built
  once with `SQLITE_PREPARE_PERSISTENT`, `SQLITE_THREADSAFE=2` with unused features left out.
- **No caching** (rule 5): every request runs its queries, and every token's HMAC is computed. The only
  thing computed once is the HMAC key schedule derived from `JWT_SECRET`.
- **Strict input handling** (shared with our epoll entry): a full RFC 8259 JSON validator (duplicate
  keys: last wins, like `JSON.parse`), `\u` escapes and surrogate pairs decoded, invalid UTF-8 rejected
  as malformed JSON, whitespace trimmed with JavaScript's `String.prototype.trim` set, and the
  500-character limit counted in code points. JWTs: HS256 only, constant-time signature compare, `exp`
  and `nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and
  `Expect: 100-continue`. Chunked request bodies aren't supported (they're out of scope).
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard
  limit it was given.

## Measurements on x86

On EC2 `m5n.large` (Xeon 8259CL, Cascade Lake like the benchmark box's 8260), booted with one CPU and 2 GB, kernel
6.8, MTU 1500, a separate load generator running the `bench/load.js` user model with k6-shaped requests, each run on a
fresh database. CPU per request counts the whole box, softirq included. Same-box runs differ by up to 3%, so these are
interleaved rounds, not separate sessions. rust-iouring-kensac is built from its own folder. The earlier entry carries
only the buffer-ring retry, without which it doesn't start on this kernel.

| 250,000 users, 3 interleaved rounds | us CPU/request, whole box | each round |
|---|---:|---|
| this entry before the B-tree reads | 23.95 | 24.43, 23.62, 23.79 |
| this entry | 20.06 | 20.38, 19.65, 20.14 |
| rust-iouring-kensac | 18.98 | 19.51, 18.65, 18.78 |

`mmap` instead of the page cache: 4% more CPU per request (one run). Batch targets: 128 completions or 2 ms was
best; 32 or 1 ms and 512 or 8 ms each cost 3 to 6%, and 256 or 4 ms was level (one run each). The earlier design
held 250,000 on the benchmark box and kensac 310,000, a ratio of 1.24; the same two here measure 1.26.

`test/test.sh` passes all 42. `kill -9` under write load, during checkpoints: all 545,300 acknowledged writes
present after restart, `integrity_check` ok.

Ideas credited to other entries: B-tree cursor reads, the `unix-excl` VFS, page cache over `mmap`, a nice-19
checkpoint thread with write holds, `SO_REUSEPORT` listeners and `DEFER_TASKRUN` on every ring from
rust-iouring-kensac (#264); the `resv[0]` retry from rust-iouring-kensac (#264) and c-iouring-pavlyhalim (#245).

## Earlier measurements (arm64, before the B-tree reads)

Measured in Docker on an Apple-silicon host, native arm64 (Rosetta has no io_uring), server limited to
one CPU, 2 GB RAM, no swap and 65,535 descriptors, load generators in separate containers. Docker's
default seccomp profile blocks the io_uring syscalls, so these containers ran with it off; the
benchmark droplet and CI run on a plain VM. Same session, alternating runs against our epoll entry,
three 15-second runs each (mean):

| wrk | epoll entry | this entry | change |
|---|---:|---:|---:|
| mixed, 64 connections | 88,600 req/s, 11.3 us CPU/req | 115,800 req/s, 8.7 us CPU/req | +31% |
| mixed, 1,024 connections | 87,500 req/s, 11.4 us CPU/req | 98,800 req/s, 10.1 us CPU/req | +13% |
| writes, 256 connections | 57,500 req/s, 17.3 us CPU/req | 69,500 req/s, 13.9 us CPU/req | +21% |

Where it comes from: the io_uring loop alone (no feed change, no PGO) is level with epoll at 64
connections (+1.5%) and 8% behind at 1,024; two thirds of the server's CPU here is the kernel's TCP
work, which is the same either way. The feed range scans take feed-only throughput from about 62k to
88k req/s (15 to 10.5 us CPU per request). PGO adds about 6% on mixed (6% less CPU per request, every
pair). k6 `bench/load.js` with 80,000 users (more than one table holds): 0% failures, p95 1 ms, p99 8 ms,
36% CPU, 170 MB; the epoll entry at the same load: p95 4.3 ms, p99 72 ms, 44% CPU, 143 MB.

Batched wake-ups, same setup, k6 `bench/load.js` with 80,000 users and think times scaled to 0.3
(about 28,000 req/s, the request rate of roughly 260,000 real users), 90-second hold, alternating
runs: 16.2, 16.4 and 16.6 us CPU per request against 20.6, 19.6, 21.5 and 21.7 without (-21%), 46%
CPU instead of 55 to 61%, p95 5 to 10 ms, 0% errors. wrk is unchanged (mixed at 64 and 1,024
connections, writes at 256: within 2%, both directions). 256 completions or 12 ms gave the same
16.5 and 16.7 us, so the shorter wait stays.

Tried and dropped: synchronous
`RESTART` checkpoints in the loop every 4,096 WAL frames (writes -40%, mixed p99 56 to 106 ms against
7 to 9 ms), and capping a batch at 128 responses (within noise).

## License

MIT, under the repo's [license](../../LICENSE).
