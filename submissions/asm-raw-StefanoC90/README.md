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
| HTTP | hand-written HTTP/1.1 server on raw `socket`/`accept`/`read`/`writev` syscalls |
| JWT | hand-written SHA-256, HMAC-SHA256 and base64url in assembly (no OpenSSL) |
| JSON | hand-written validating JSON reader in assembly |
| OS | Ubuntu 24.04 x86_64 |

## Nginx or direct

**Behind Nginx** (`bench/nginx.conf`, listening on `127.0.0.1:3000`). The server
is a thread-per-connection design sized for Nginx's pool of at most 64 idle
upstream connections, not for 15,000 direct keep-alive clients. Idle connections
are held for 75 s (`SO_RCVTIMEO`), above the 65 s the Nginx config needs.

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

- **64 worker threads**, each looping on `accept()` on one shared listening
  socket. This matches Nginx's 64 idle upstream sockets; no per-connection
  thread creation at request time.
- **One SQLite connection per worker**, opened once with `busy_timeout=5000`.
  Five statements are prepared once per connection (`feed`, `get`, `insert
  post`, `post exists`, `insert like`, plus `SELECT 1` for health) and reset
  per use. Prepared statements cache query plans, not data.
- **A single `writev`** sends response headers and body together, with
  `TCP_NODELAY` set on every accepted socket.
- **Keep-alive with pipelining**: leftover bytes after a request are moved to
  the front of the buffer and parsed as the next request; `Connection: close`
  and HTTP/1.0 are honoured.
- **SQLite settings**: `journal_mode=WAL`, `synchronous=NORMAL`,
  `busy_timeout=5000`, `cache_size=-1024` (1 MiB/connection),
  `mmap_size=67108864`, plus `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` at compile time.
  Every write steps the statement to `SQLITE_DONE` (committing the implicit
  transaction) before the response is written.
- **No cross-request caching**: every request hits SQLite; JWT signatures are
  recomputed for every authenticated request.
- **Hand-rolled crypto**: SHA-256 with a message schedule in registers/stack,
  HMAC-SHA256 with constant-time comparison, and a base64url decoder — avoiding
  an OpenSSL dependency and its startup cost.
- **Hand-rolled JSON**: a strict validating scanner/decoder that decodes
  escapes and `\uXXXX` (including surrogate pairs) into a per-worker buffer, so
  the round-tripped body and character-count limit are exact.
- **Minimal allocations**: per-worker request/response/temp/user buffers are
  allocated once at startup; the request and response paths make no heap
  allocations.
- Response bodies are built directly into a 512-byte-headroom buffer, so no
  intermediate serialization copies are made.
