# Zig + io_uring + SQLite

| | |
|---|---|
| Language | Zig 0.15.2 (official Linux tarball, SHA-256 pinned in `install.sh`), `ReleaseFast`, `x86_64_v2` |
| Framework | none: one `io_uring` event loop on `std.os.linux.IoUring`, hand-written HTTP/1.1, JSON and JWT code |
| SQLite | 3.54.1 official amalgamation (SHA-256 pinned in `build.sh`), compiled by GCC 13.3 (Ubuntu 24.04's `build-essential`) with `-O2 -march=x86-64-v2`, profile-guided, and linked into the binary |
| Crypto | HMAC-SHA256 in `src/main.zig` (a plain SHA-256 loop, checked against `std.crypto` by a `zig test` block) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

No third-party Zig packages; io_uring comes from the Zig standard library. Needs Linux 6.1 or newer
with io_uring enabled (Ubuntu 24.04's 6.8 kernel has it on by default).

## Running it

```bash
sudo bash install.sh   # Zig 0.15.2 into /opt, plus build-essential/curl/unzip/xz
bash build.sh          # fetches + verifies SQLite, builds bin/server (about a minute)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
zig test -lc -Ideps/sqlite-amalgamation-3540100 src/main.zig   # optional: HMAC self-check
```

## Design

The HTTP, JSON, JWT and SQLite code is the same as our Zig epoll entry; the event loop is a port of
our C io_uring entry.

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
  and accepts there. With `LimitNOFILE=65535` that is about 2 million clients in one process, with no
  second worker process. Extra rings signal an eventfd that ring 0 reads, so one wait covers all of them.
- **Accepts are single-shot, 256 in flight.** A multishot accept can't be stopped before its table
  fills, and the accept that finds the table full has already taken the connection off the queue, so
  the kernel can only reset it. Single-shot accepts are only issued while a table has room. They are
  rearmed once per loop pass, so the number in flight caps accepts per pass: with 16, a 330,000-user
  ramp was accepted at only ~1,700 connections/s.
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
- **Profile-guided SQLite.** Zig has no PGO of its own, so `build.sh` compiles SQLite with GCC twice:
  first instrumented, linked with `train.c`, which creates a synthetic database (20k users, 200k
  posts, 600k likes; the schema's tables and indexes, none of the challenge seed) and runs the server's
  statements 300,000 times in the benchmark mix, with a commit every 64; then again with that profile.
  The optimized `sqlite3.o` is linked into the Zig binary. The trainer and its database are deleted.
- **Our own SHA-256.** Without CPU SHA instructions (`x86-64-v2` has none) the standard library's
  SHA-256 measured 4 to 7 times slower than a plain loop, and every token check hashes 4 to 6 blocks:
  it was a quarter of the server's CPU on writes. `src/main.zig` has a 50-line SHA-256 and HMAC with
  the `key^ipad` / `key^opad` states computed once; `zig test` checks it against `std.crypto`.
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, prepared
  statements built once with `SQLITE_PREPARE_PERSISTENT`. SQLite is compiled single-threaded with
  unused features left out. A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON
  CONFLICT DO NOTHING`, with a post lookup only when nothing was inserted.
- **JSON escaping a vector at a time.** Response strings are scanned 16 bytes per step (SSE2 / NEON)
  for the next byte that needs escaping, and the clean run before it is copied in one go: a feed's
  strings (about 3.3 KB) took 0.44 us instead of 2.1 us, and wrk's mixed load used 5% less CPU per
  request (both measured on arm64).
- **No caching** (rule 5): every request runs its queries, and every token's HMAC is computed. The only
  thing computed once is the HMAC key schedule derived from `JWT_SECRET`.
- **Strict input handling**: a full RFC 8259 JSON validator (duplicate keys: last wins, like
  `JSON.parse`), `\u` escapes and surrogate pairs decoded, invalid UTF-8 rejected as malformed JSON,
  whitespace trimmed with JavaScript's `String.prototype.trim` set, and the 500-character limit counted
  in code points. JWTs: HS256 only, constant-time signature compare, `exp` and `nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and
  `Expect: 100-continue`. Chunked request bodies aren't supported (they're out of scope).
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard
  limit it was given.

## Local measurements

Measured in Docker on an Apple-silicon host, native arm64 (Rosetta has no io_uring), server limited to
one CPU, 2 GB RAM and 65,535 descriptors, load generators in separate containers. Docker's default
seccomp profile blocks the io_uring syscalls, so these containers ran with it off; the benchmark
droplet runs on a plain VM. Same session, alternating runs, three 15-second runs each (mean), server
CPU per request from its cgroup. Other jobs shared the host, so treat the numbers as rough.

| wrk, CPU per request | Zig epoll entry | this entry | our C io_uring entry |
|---|---:|---:|---:|
| mixed, 64 connections | 11.4 us (86,200 req/s) | 10.5 us (94,100 req/s) | 9.9 us (96,800 req/s) |
| mixed, 1,024 connections | 11.3 us (89,200 req/s) | 10.3 us (93,600 req/s) | 10.1 us (98,900 req/s) |
| writes, 256 connections | 17.4 us (57,500 req/s) | 12.1 us (82,500 req/s) | 12.7 us (78,300 req/s) |

(The epoll column is before it got the SHA-256 change below.) Where it comes from, same method: the
io_uring loop by itself was level with epoll (+5% CPU at 64 connections, -1% at 1,024, -4% on writes).
The SHA-256 change takes writes from 17.4 to 12.1 us per request (-30%); the standard library's
SHA-256 was 27% of the server's CPU in a `perf` profile of the write mix. PGO of SQLite cut 3 to 7%
in five of six pairs. 5,000 connections with `nofile=1024` are served by one process with 13
descriptors open (five rings).

Batched wake-ups, same setup, k6 `bench/load.js` with 80,000 users and think times scaled to 0.3
(about 28,000 req/s), 90-second hold, alternating runs: 16.9 and 16.4 us CPU per request against
18.3 and 19.0 without (-11%), 46 to 47% CPU instead of 51 to 54%, 0% errors.

## License

MIT, under the repo's [license](../../LICENSE).
