# Rust + io_uring + SQLite

| | |
|---|---|
| Language | Rust 1.90.0 (edition 2021), installed with rustup into `/opt/rust` |
| Framework | none: one `io_uring` event loop, hand-written HTTP/1.1, JSON and JWT code |
| Crates | `io-uring` 0.7.15 and `libc` 0.2.177 (all versions pinned in `Cargo.lock`) |
| SQLite | 3.54.1, official amalgamation (SHA-256 checked), compiled by `build.sh` and linked statically |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80`) itself |

Needs Linux 6.1 or newer with io_uring enabled (Ubuntu 24.04's 6.8 kernel has it on by default).

```bash
sudo bash install.sh   # build-essential, curl, unzip, rustup + Rust 1.90.0
bash build.sh          # SQLite static lib, then cargo build --release --locked
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

The request handling (HTTP parsing, JSON, JWT, SQL, responses) is the same code as our
`rust-epoll-unorig-username` entry; the I/O is the design of our `c-iouring-unorig-username` entry.

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
  worker processes. Extra rings signal an eventfd that ring 0 reads, so one wait covers all of them.
- **Accepts are single-shot, 16 in flight.** A multishot accept can't be stopped before its table
  fills, and the accept that finds the table full has already taken the connection off the queue, so
  the kernel can only reset it. Single-shot accepts are only issued while a table has room (never
  into its last 16 slots).
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
  operations and closes the slot in one linked pair; generation numbers in the completion tags make
  completions for a reused slot harmless.
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
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, normal locking,
  prepared statements built once with `SQLITE_PREPARE_PERSISTENT`. SQLite is compiled single-threaded
  with unused features left out. A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON
  CONFLICT DO NOTHING`, with a post lookup only when nothing was inserted.
- **No caching** (rule 5): every request runs its queries, and every token's HMAC is computed. The only
  thing computed once is the HMAC key schedule derived from `JWT_SECRET`.
- **Strict input handling**: a full RFC 8259 JSON validator (duplicate keys: last wins, like
  `JSON.parse`), `\u` escapes and surrogate pairs decoded (lone surrogates become U+FFFD), invalid UTF-8
  rejected as malformed JSON, whitespace trimmed with JavaScript's `String.prototype.trim` set, and the
  500-character limit counted in code points. JWTs: HS256 only, constant-time signature compare, `exp`
  and `nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and
  `Expect: 100-continue`. Chunked request bodies aren't supported (they're out of scope).
- **Build**: `opt-level=3`, fat LTO, one codegen unit, `panic=abort`, `target-cpu=x86-64-v2`.
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard
  limit it was given.

## Local measurements

Measured in Docker on an Apple-silicon host, native arm64 (Rosetta has no io_uring), server limited to
one CPU, 2 GB RAM, no swap and 65,535 descriptors, wrk in a separate container, other jobs running on
the host (numbers are rough). Docker's default seccomp profile blocks the io_uring syscalls, so these
containers ran with it off. Same session, alternating runs, three 15-second runs each (mean):

| wrk | our Rust epoll entry | this entry | our C io_uring entry |
|---|---:|---:|---:|
| mixed, 64 connections | 94,500 req/s, 10.6 us CPU/req | 95,400 req/s, 10.4 us CPU/req | 100,100 req/s, 9.9 us CPU/req |
| mixed, 1,024 connections | 89,900 req/s, 10.8 us CPU/req | 88,900 req/s, 11.2 us CPU/req | 91,400 req/s, 10.8 us CPU/req |
| writes, 256 connections | 66,200 req/s, 14.0 us CPU/req | 69,900 req/s, 13.8 us CPU/req | 71,600 req/s, 13.5 us CPU/req |

Per request the io_uring loop costs about what epoll does (two thirds of the server's CPU is the
kernel's TCP work either way). What it buys is one process for any number of clients: 5,000 keep-alive
connections at `nofile=1024` are served by one process with 13 descriptors (5 rings, 135 MB RSS),
where the epoll entry forks a worker per descriptor limit. The feed range scans took the epoll entry
from 13.4 to 11.8 us CPU per mixed request in the same session.

Tried and dropped: a profile-guided SQLite build (an instrumented SQLite running the server's
statements on a synthetic database, then recompiled with the profile). Paired against the plain
build it was no better: 9.8 against 9.5 us CPU per mixed request over 8 pairs, level on writes.

## License

MIT, under the repo's [license](../../LICENSE).
