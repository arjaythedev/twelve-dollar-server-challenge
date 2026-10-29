# x86-64 assembly (NASM) + SQLite, io_uring, direct

A feed API server written in x86-64 assembly. Every piece of application logic is hand-written
assembly: the HTTP/1.1 parser (keep-alive, pipelining, `Content-Length`, `Connection`, `Expect:
100-continue`, HTTP/1.0), routing, a strict RFC 8259 JSON validator with full string decoding
(`\uXXXX` and surrogate pairs), base64url, SHA-256 and HMAC-SHA256, the JWT checks, JSON output with
escaping, integer formatting, and the io_uring event loop. There is no liburing: the rings are set
up with raw `io_uring_setup`, `io_uring_register` and `mmap` syscalls and driven with
`io_uring_enter`. The only external code is SQLite.

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 `nasm`) |
| Assembler / linker | `nasm -f elf64`, linked with `gcc` (libc provides `_start`/CRT so SQLite's C runtime initializes) |
| SQLite | 3.54.1, official amalgamation, SHA-256 pinned, compiled from source by `build.sh` |
| **Nginx or direct** | **Direct**: the server speaks HTTP/1.1 on `HOST:PORT` itself, no Nginx |
| Dependencies | one (SQLite). No HTTP library, JSON library, crypto library, io_uring library or framework. |

Needs Linux 6.1 or newer with io_uring enabled (Ubuntu 24.04's 6.8 kernel has it on by default).

## Build and run

```bash
sudo bash install.sh     # apt: build-essential, nasm, curl, unzip
bash build.sh            # downloads + verifies SQLite, assembles, links bin/server
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`main` is the entry point (so the C runtime SQLite relies on is set up); it reads the four
environment variables itself from `envp`, and calls only `sqlite3_*` functions from libc. Per-connection
buffers come from `sqlite3_malloc64`/`sqlite3_realloc64`/`sqlite3_free`.

## Design

It mirrors our C io_uring server (`submissions/c-iouring-unorig-username`).

- **One process, one thread, one io_uring loop.** The box has one vCPU, so threads would only add
  context switches. Each loop iteration is one `io_uring_enter` that submits everything queued and
  waits for completions (`DEFER_TASKRUN`, `SINGLE_ISSUER`, so completions are processed in that call).
- **Wake-ups are batched when busy.** If the last pass handled 4 or more completions, the wait asks for
  128 completions (capped at 40% of open connections, so a few clients without think time never sit
  out the timer) or 6 ms, whichever comes first; otherwise it wakes on the first one. Each wake-up then
  carries more requests, so the syscalls, the batch's `COMMIT` and the send submissions are shared by
  more of them. It only delays when responses are sent, never what they contain: every response still
  waits for its batch's commit.
- **Accepted sockets are registered (direct) descriptors, not process descriptors.** Accepts use
  `IORING_FILE_INDEX_ALLOC`, so a client occupies a slot in the ring's sparse registered file table
  and the process descriptor table only holds the listener, the rings and SQLite's files. The kernel
  caps one table at `RLIMIT_NOFILE`, so when a table is full the loop adds another ring (same thread,
  up to 32) and accepts there. With `LimitNOFILE=65535` that is about 2 million clients in one
  process. Extra rings signal an eventfd that ring 0 reads, so one wait covers all of them.
- **Accepts are single-shot, 16 in flight.** A multishot accept can't be stopped before its table
  fills, and the accept that finds the table full has already taken the connection off the queue, so
  the kernel can only reset it. Single-shot accepts are only issued while a table has room, and the
  last 16 slots are never used (closes still in flight may hold them).
- **Multishot receives into provided buffer rings** (4,096 x 2 KB per ring). A receive stays armed for
  the life of the connection; each chunk arrives in a kernel-picked buffer that goes back to the ring as
  soon as it is parsed. Idle clients hold no buffer. A partial request is copied into a per-connection
  buffer until the rest arrives. If the ring ran dry (`ENOBUFS`) the receive is rearmed after the batch.
- **A transaction per completion batch (group commit).** The first request in a batch that touches the
  database opens a deferred `BEGIN`; every read and write in the batch runs inside it, and the batch
  ends with one `COMMIT`. Responses are written into a 4 MiB arena and only handed to `send` SQEs after
  that `COMMIT` returns, so a `201` always means the row is committed (rule 6). If a commit fails,
  nothing from that batch is sent: those connections are closed instead.
- **Sends go through io_uring**, one in flight per connection so responses stay in order (later ones
  queue behind it). Each send owns a copy of its bytes, so the arena is reused immediately and a slow
  reader can never pin it. Short sends resubmit the rest. Closing a connection cancels its pending
  receive (and send, if one is in flight) and closes the slot in one linked chain; generation numbers in
  the completion tags make completions for a reused slot harmless. The cancels name the operations by
  their completion tags, which the kernel looks up in a hash table. Cancelling by descriptor instead
  walks every pending operation on the ring (one receive per open connection): with 20,000 idle
  clients that made each close cost about 300 us of CPU instead of about 20 us (measured on our C and
  Zig io_uring entries, which close connections the same way; this entry can't run on our arm64 host).
- **Feed: newest ids first, then two range scans.** `/feed` first reads the 20 newest ids from the
  `(created_at, id)` index. When they span fewer than 256 ids (new posts get consecutive ids, so this
  is the normal case), one scan of `likes_post_id_idx` over that range counts the likes and one scan
  of `posts` over it fetches the rows, which are then put back in feed order. That replaces 20
  separate count subqueries and 20 join lookups. If the ids are spread out, or a row doesn't turn up,
  it runs the reference query instead. Both run in the batch's read transaction, so they see one
  snapshot.
- **Checkpoints happen off the request path.** A small helper process (the same binary, fork+exec'd
  as `server --ckpt`) runs `sqlite3_wal_checkpoint_v2` `PASSIVE` in a loop, sleeping 1 ms while it is
  behind and 10 ms otherwise, so the event loop never waits on the disk. If the helper can't keep up
  (CPU pegged for minutes), the server's own `wal_autocheckpoint=65536` still caps the WAL at about
  256 MB. Durability is unchanged: WAL with `synchronous=NORMAL`, and a response waits for its `COMMIT`.
- **No caching across requests.** Every request reads SQLite while it is served. Prepared statements
  are reused (query plans only, not data). Every JWT signature is verified on every request with
  hand-written HMAC-SHA256; there is no verification cache.
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, prepared
  statements built once with `SQLITE_PREPARE_PERSISTENT`, compiled single-threaded with unused
  features left out. A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT
  DO NOTHING`, with a post lookup only when nothing was inserted.
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the
  hard limit it was given.

### Correctness notes

- JSON strings that do not fit their decode buffer (for example an unknown key longer than 32
  bytes) are still validated through their closing quote, so they never turn a valid document into a
  400.
- JSON string escaping checks 16 bytes per step with SSE2 (`pminub`/`pcmpeqb`/`pmovmskb`) and copies
  the clean run before the next byte that needs escaping in one go.
- JSON parsing is strict (rejects trailing bytes, bad escapes, control characters, invalid UTF-8).
  Request-body string length is counted in Unicode code points for the 500-character limit.
- Body trimming removes the same whitespace set as JavaScript `String.trim` (ASCII plus
  U+00A0, U+1680, U+2000 to U+200A, U+2028/2029, U+202F, U+205F, U+3000, U+FEFF).
- On authenticated routes, auth is checked before the id, matching the spec.
- Every call into SQLite is made with a 16-byte aligned stack, as the System V ABI requires.
- The stack is marked non-executable (`.note.GNU-stack`).

### Limitations

- `Transfer-Encoding` (chunked) request bodies are not supported (out of scope per SPEC.md); such a
  request closes the connection.

## Local verification

Rosetta has no io_uring, so this was checked on a real x86-64 kernel (Linux 6.8 under full-system
emulation), in Docker with the default seccomp profile off (it blocks the io_uring syscalls; the
benchmark droplet is a plain VM): `test/run.sh` passes 42/42, and one server process with
`nofile=1024` served 10,000 requests on 5,000 open keep-alive connections across 5 rings, holding
13 descriptors. Emulation makes throughput numbers meaningless, so none are given here.

## License

MIT, under the repo's [license](../../LICENSE).
