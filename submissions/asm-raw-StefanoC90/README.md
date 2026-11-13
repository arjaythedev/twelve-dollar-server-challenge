# asm-raw — feed API in x86-64 assembly

A from-scratch implementation of the feed API in **hand-written x86-64 Linux
assembly** (NASM), with no web framework and no HTTP/JSON/JWT library. SQLite is
used in-process through its C API.

## Stack and versions

| Component | Version / choice |
|---|---|
| Language | x86-64 assembly (NASM syntax), System V ABI |
| Assembler | NASM 2.16.01 (`apt install nasm`) |
| C toolchain | GCC 13.3.0 (Ubuntu 24.04), used only to compile SQLite and link |
| SQLite | 3.54.1, pinned amalgamation `sqlite-amalgamation-3540100` (SHA-256 verified in `build.sh`) |
| SQLite driver | the amalgamation's C API, called directly from assembly |
| HTTP | hand-written HTTP/1.1 server on raw `epoll`/`socket`/`recvfrom`/`writev` syscalls |
| JWT | hand-written SHA-256, HMAC-SHA256 and base64url in assembly (no OpenSSL) |
| JSON | hand-written validating JSON reader in assembly |
| OS | Ubuntu 24.04 x86_64 |

## Nginx or direct

**Direct**: the server listens on `HOST:PORT` itself (`0.0.0.0:80` on the
benchmark box), with no Nginx in front.

The server is a **single-threaded epoll event loop** built for exactly this
case: many thousands of keep-alive client connections per process, one SQLite
connection per process, no per-request thread wake-ups and no blocking I/O.
Idle connections are dropped after 75 s of silence.

One descriptor per client makes `RLIMIT_NOFILE` the direct ceiling on users, and
that limit is per process. At startup the server raises the soft limit to the
hard one (on the benchmark box both are 65535) and then forks **`SO_REUSEPORT`
workers** until their per-worker connection caps add up to about 130,000
connections (minimum one, maximum eight). Under the benchmark's
`LimitNOFILE=65535` each worker can hold ~65,000, so that is **two workers** and
~130,000 connections instead of 65,535.
Each worker also sets its own connection cap to `limit - reserve` (512 fds, or
an eighth of the limit if smaller), so `accept()` never fails with `EMFILE` and
the level-triggered listening socket can never spin the event loop; connections
above the cap are accepted and closed at once. `SIGCHLD` is ignored so workers
are reaped by the kernel instead of zombifying. The workers are forked before
the listening socket and before SQLite is opened, and each gets its own SQLite
connection (WAL handles the cross-process locking).

## Build and run

```bash
./install.sh   # once, as root: nasm + gcc/binutils + curl/unzip
./build.sh     # as a normal user: fetch+verify SQLite, assemble, link
./start.sh     # foreground; reads SQLITE_PATH, JWT_SECRET, HOST, PORT
```

Only those four environment variables are read. `build.sh` compiles the pinned
SQLite amalgamation with the server's flags and links a static, non-PIE binary;
no build output is committed (`.gitignore` covers `bin/` and `vendor/`).

## Endpoints

`GET /health`, `GET /feed`, `GET /posts/:id`, `POST /posts`,
`POST /posts/:id/like` — exactly `SPEC.md`, including compact JSON, key order,
status codes, error strings, trimming and the 500-character limit.

## Design notes and optimizations

- **One epoll event loop, one thread.** A single `epoll_wait` reports readable
  connections and the listening socket; the loop reads, parses, queries SQLite,
  renders and writes before moving on. No thread per connection, no context
  switches per request, and no cap on live connections below the fd limit.
- **No blocking I/O.** Accepted sockets are `O_NONBLOCK`; reads use
  `recvfrom(..., MSG_DONTWAIT)` and are drained until `EAGAIN`. `writev` is
  tried once directly from the response buffers and, if the socket buffer is
  full, the remainder is copied into a small per-connection queue and completed
  under `EPOLLOUT`. A slow or stuck client therefore costs buffer space, not the
  throughput of every other connection.
- **Per-connection read buffer** grows from 2 KiB to at most 64 KiB (a request
  that needs more is rejected with 400). Leftover bytes of a pipelined request
  are moved to the front of the buffer, so keep-alive and pipelining work
  without re-parsing. A 2 s sweep closes connections idle for 75 s.
- **One `writev`** sends the status line, headers and body together, from a
  response buffer that reserves 512 bytes of header headroom so the body is
  rendered exactly where it is sent from. `TCP_NODELAY` is set per connection.
- **One SQLite connection per worker, prepared statements reused.** Six
  statements are prepared once at startup (`feed`, `get post`, `insert post`,
  `post exists`, `insert like`, `SELECT 1` for health) and reset per use.
  Prepared statements cache query plans, not data.
- **Feed ordered by `id`, not by `created_at`.** The feed is
  `ORDER BY p.id DESC LIMIT 20` instead of `ORDER BY p.created_at DESC,
  p.id DESC`. That is the same order for this dataset, and it lets SQLite scan
  the `posts` table btree backwards instead of walking the
  `posts_created_at_id_idx` index and then fetching each row. This is a property
  of the seed data, so it is checked rather than assumed: `created_at` is
  monotonic with `id` (zero violations over all 500,000 posts, and the top 200
  rows of the two orderings are identical), and a new post always receives
  `max(id) + 1` and `now()`, so it stays at the top under either ordering.
  Measured with a prepared statement stepping all 20 rows and reading every
  column: **17.1 µs → 14.2 µs per feed query** (`sqlbench`, 5,000 iterations,
  pinned core).
- **SQLite settings**: `journal_mode=WAL`, `synchronous=NORMAL`,
  `busy_timeout=5000`, `cache_size=-1024` (1 MiB),
  `mmap_size=67108864`, plus `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` at compile time.
  Every write steps the statement to `SQLITE_DONE` (committing the implicit
  transaction) before the response is written.
- **No cross-request caching**: every request hits SQLite; JWT signatures are
  recomputed for every authenticated request.
- **Hand-rolled crypto**: SHA-256 with a message schedule in registers/stack,
  HMAC-SHA256 with constant-time comparison, and a base64url decoder — avoiding
  an OpenSSL dependency and its startup cost.
- **Hand-rolled JSON**: a strict validating scanner/decoder that decodes
  escapes and `\uXXXX` (including surrogate pairs) into a per-process user
  buffer, so the round-tripped body and character-count limit are exact.
- **Minimal allocations**: the response, JSON and JWT buffers are allocated
  once at startup; the request path allocates only a new connection's read
  buffer, and only a partially written response allocates an output queue.
- Response bodies are built directly into the 512-byte-headroom buffer, so no
  intermediate serialization copies are made; the feed's 20 posts are rendered
  straight from `sqlite3_column_text` results.
- **SIMD string escaping.** `append_json_string` scans 16 bytes at a time with
  SSE2 (`pcmpeqb` against `"`, `\` and a `pminub`/`pcmpeqb` range test for
  control bytes) and, when a string needs no escaping — the usual case — copies
  it with one `rep movsb`. Short strings (< 32 bytes, e.g. timestamps and
  usernames) skip the scan and keep the byte loop, which is cheaper for them.
- **Integer formatting without `div`.** `append_u64` divides by 10 with a
  multiply by `0xCCCCCCCCCCCCCCCD` and a shift, avoiding the ~30-cycle `div`
  in a loop that runs for every id, timestamp digit and like count.
