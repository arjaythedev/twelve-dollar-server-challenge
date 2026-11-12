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
case: many thousands of keep-alive client connections in one process, one
SQLite connection, no per-request thread wake-ups and no blocking I/O. Idle
connections are dropped after 75 s of silence, and the process stays inside the
`LimitNOFILE=65535` budget (one descriptor per client).

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
- **One SQLite connection, prepared statements reused.** Six statements are
  prepared once at startup (`feed`, `get post`, `insert post`, `post exists`,
  `insert like`, `SELECT 1` for health) and reset per use. Prepared statements
  cache query plans, not data.
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
