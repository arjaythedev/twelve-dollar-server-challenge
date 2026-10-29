# C + io_uring + SQLite

| | |
|---|---|
| Language | C (GCC 13.3, Ubuntu 24.04's `build-essential`), `-O2 -march=x86-64-v2 -flto`, profile-guided |
| Framework | none: one `io_uring` event loop, hand-written HTTP/1.1, JSON and JWT code |
| io_uring | liburing 2.12, built from the release tarball (SHA-256 pinned in `build.sh`) |
| SQLite | 3.54.1, the official amalgamation (SHA-256 pinned in `build.sh`), compiled into the binary |
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

- **One process, one thread, one `io_uring` loop.** The box has one vCPU, so threads would only add
  context switches. Each loop iteration is one `io_uring_enter` that submits everything queued and
  waits for completions (`DEFER_TASKRUN`, `SINGLE_ISSUER`, so completions are processed in that call).
- **Wake-ups are batched when busy.** If the last pass handled 4 or more completions, the wait asks for
  128 completions (capped at 40% of open connections, so a few clients without think time never sit
  out the timer) or 6 ms, whichever comes first; otherwise it wakes on the first one. Each wake-up then
  carries more requests, so the syscalls, the batch's `COMMIT` and the send submissions are shared by
  more of them. It only delays when responses are sent, never what they contain: every response still
  waits for its batch's commit.
- **Accepted sockets are registered (direct) descriptors, not process descriptors.** Accepts use
  `IORING_FILE_INDEX_ALLOC`, so a client occupies a slot in the ring's registered file table and the
  process descriptor table only holds the listener, the rings and SQLite's files. The kernel caps one
  table at `RLIMIT_NOFILE`, so when a table is full the loop adds another ring (same thread, up to 32)
  and accepts there. With `LimitNOFILE=65535` that is about 2 million clients in one process, instead
  of forking a new worker every 65,535. Extra rings signal an eventfd that ring 0 reads, so one wait
  covers all of them.
- **Accepts are single-shot, 256 in flight.** A multishot accept can't be stopped before its table
  fills, and the accept that finds the table full has already taken the connection off the queue, so
  the kernel can only reset it (measured: the client at slot 1,025 got `ECONNRESET`). Single-shot
  accepts are only issued while a table has room. They are rearmed once per loop pass, so the number
  in flight caps accepts per pass: with 16, a 330,000-user ramp was accepted at only ~1,700
  connections/s.
- **Multishot receives into provided buffer rings** (4,096 x 2 KB per ring). A receive stays armed for
  the life of the connection; each chunk arrives in a kernel-picked buffer that goes back to the ring as
  soon as it is parsed. Idle clients hold no buffer. A partial request is copied into a per-connection
  buffer until the rest arrives. If the ring ran dry (`ENOBUFS`) the receive is rearmed after the batch.
- **A transaction per completion batch (group commit).** The first request in a batch that touches the
  database opens a deferred `BEGIN`; every read and write in the batch runs inside it, and the batch
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
- **Feed: newest ids first, then two range scans.** `/feed` first reads the 20 newest ids from the
  `(created_at, id)` index. When they span fewer than 256 ids (new posts get consecutive ids, so this is
  the normal case), one scan of `likes_post_id_idx` over that range counts the likes and one scan of
  `posts` over it fetches the rows, which are then put back in feed order. That replaces 20 separate
  count subqueries and 20 join lookups. If the ids are spread out, or a row doesn't turn up, it runs the
  reference query instead. Both run in the batch's read transaction, so they see one snapshot.
- **Checkpoints happen off the request path.** A small helper process (the same binary, started with
  `--ckpt`) runs `PASSIVE` checkpoints in a loop, so the event loop never waits on the disk. If the
  helper can't keep up (CPU pegged for minutes), the server's own `wal_autocheckpoint=65536` still caps
  the WAL at about 256 MB. Durability is unchanged: WAL with `synchronous=NORMAL`, and a response waits
  for its `COMMIT`.
- **Profile-guided build.** `build.sh` first builds an instrumented binary with `train.c` compiled in.
  It creates a synthetic database (20k users, 200k posts, 600k likes; the schema's tables and indexes,
  none of the challenge seed) and serves 300,000 requests in the benchmark mix from memory: parsing,
  JWT checks, queries, writes and commits. The real binary is then compiled with that profile. The
  training binary and database are deleted; `train.c` is not in the final binary.
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, prepared
  statements built once with `SQLITE_PREPARE_PERSISTENT`. SQLite is compiled single-threaded with
  unused features left out. A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON
  CONFLICT DO NOTHING`, with a post lookup only when nothing was inserted.
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

## Local measurements

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
