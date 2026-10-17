# Architecture

This document explains how the submission works, in enough depth to read the
code with understanding. See the [README](../README.md) for the quick start.

## Shape of the system

One process. One thread. One event loop. HTTP/1.1 is served directly on
`:80`/`:3000` (no reverse proxy) and every read and write goes to **SQLite**
linked in-process.

```
        io_uring completion queue
               │
      ┌────────┴────────┐
 listen fd         client fd
 multishot accept  recv / send
 (fixed fds)       (buf pool)
                   │
                   ▼
        http.nim  HTTP/1.1 parse + framing
                   │
                   ▼
        api.nim  routing · auth-before-id · exact errors
          │   │
   jwt.nim│   │jsonbuf.nim  byte-level JSON writer
          ▼   ▼
        db.nim ── sqlite3.nim (raw C API) ── vendored SQLite (WAL)
                 │
                 ▼
        one write per response (backpressure / registered-fd send)
```

## Why one thread

The benchmark box has one vCPU. Extra threads add context switches, cache
pressure and latency jitter without adding throughput, so the whole server is a
single thread driving a single io_uring ring. This also removes all shared state
between request handlers, which is what lets the rest of the code stay simple:
no locks, no per-request object graph, no worker hand-off.

## Modules

| File | Responsibility |
|---|---|
| `src/server.nim` | the `io_uring` transport and the socket scaffold: multishot accept/recv, provided-buffer pool, registered fds, group commit, plus `raiseNofile`, listening-socket setup and the `processRequests` loop |
| `src/http.nim` | transport-agnostic HTTP/1.1 request parsing + response framing |
| `src/iouring.nim` | self-contained raw `io_uring` bindings (no liburing) |
| `src/api.nim` | routing + the five endpoints + exact error semantics |
| `src/db.nim` | all SQL; prepared statements; pragmas; read/write lifecycle |
| `src/sqlite3.nim` | minimal raw `{.importc.}` bindings to the vendored SQLite C API |
| `src/jwt.nim` | base64url + HS256 (nimcrypto), constant-time compare |
| `src/jsonbuf.nim` | byte-level JSON writing and minimal request-body parsing |
| `src/model.nim` | the shared `Post` value type |
| `src/config.nim` | environment-only configuration |

`server.nim` is the only module that knows about sockets, and it uses the HTTP
parsing and response framing in `http.nim`. The fd-limit bump, the
listening-socket setup and the `processRequests` request/response loop live in
the same `server.nim` module. `api.nim` takes plain strings in and returns a
status + body, so the server and the unit tests share one implementation of the
API.

## Request lifecycle

1. **accept**: a multishot accept installs each accepted socket directly into
   the ring's fixed-file table (or, when direct accept is unavailable, into the
   next free slot after an explicit `accept`).
2. **read**: `recv` until `EAGAIN`. Bytes are appended to the connection's input
   buffer. If unparsed input exceeds `MaxRequest`, the connection is
   served-then-closed (see *Bounds*).
3. **parse**: the buffer is walked with a cursor (no per-request front delete).
   Headers, method, path, `Authorization`, `Content-Length` and `Connection` are
   extracted in place. Malformed / non-conforming framing → `400` + close.
4. **route + auth**: `api.handleRequest`. On authenticated routes the bearer
   token is verified **before** the id is parsed, so `POST /posts/abc/like`
   without a token is `401`, not `400`.
5. **SQLite**: one prepared statement per query, executed on the single
   connection. Single-row readers reset the statement immediately so the read
   transaction does not linger; `feed` steps to `SQLITE_DONE`.
6. **respond**: the response is appended to the output buffer and a send is
   submitted. If it cannot be sent in full, it stays buffered and the send is
   re-submitted until the socket accepts it.
7. **keep-alive**: unless `Connection: close`, the connection stays open; the 1 s
   age-based sweep (idle 75 s / stalled 30 s) manages its lifetime.

## Event-loop invariants

These are the rules that make the single-threaded completion loop correct:

- **Read to `EAGAIN`.** Multishot recv keeps consuming until `EAGAIN`; a
  completion re-arms the next recv, so no readiness edge is lost.
- **Backpressure on send.** A partial send keeps the remainder buffered and
  re-submits the send, so an already-writable socket is never missed.
- **Free the fixed-file slot before reusing it**, and guard double-close with an
  `active` flag.
- **Never block.** Every operation is submitted asynchronously; `EAGAIN` is
  never spun on.
- **Bounded work per event.** Input is capped, so a single connection cannot
  monopolise the loop.

## Connection limits (fd, conntrack)

The practical ceiling on simultaneous connections is set by the kernel, and only part of it is under
the server's control:

- **Process file descriptors.** The service runs with `LimitNOFILE = 65535` on the target box.
  Registering the fixed-file table charges every entry against `RLIMIT_NOFILE`, so the table is sized
  at startup as `min(hard - 64, 65536)`: a 64-fd reserve covers the process's own descriptors (stdio,
  the ring, the database) and `CAP = 65536` bounds the table. Once registered, each accepted socket
  lives in the table and holds **no** new process fd, so the per-connection fd wall is gone; the table
  size itself is still bounded by the limit. If the kernel refuses the requested size, startup walks
  the table down to a floor (`SlotsFloor = 16384`) instead of exiting.
- **Kernel conntrack.** `nf_conntrack_max` is about 64k on the 2 GB droplet and is machine-wide. It is
  **not** lifted by registered fds, or by anything the server can do in-process: it is the practical
  ceiling for simultaneously established connections on the target box. Local loopback tests do not
  engage conntrack, so this figure is argued from the target environment, not demonstrated locally.
- **Large listen backlog** (4096) for reconnect bursts.
- **EMFILE backoff**: on an `accept` failure the acceptor is paused and retried on the next 1 s tick;
  it never accept-spins.
- **io_uring registered fds (default)**: sockets in the fixed-file table hold no new process fd.
- **Provided-buffer receive pool (default)**: multishot recv draws from a shared, lazily-faulted pool
  (64 buffers of 8 KiB), so idle connections hold no recv buffer.
- **Age-based sweep.** A 1 s sweep (driven by the io_uring timeout) closes a connection that has been
  idle for 75 s or stalled mid-request for 30 s, keeping the open-connection count bounded over a long
  run.

The sweep is age-based, not count-based: there is no high-water connection watermark and no RST close.
A connection is closed only when it has been idle past the keep-alive timeout or stalled past the
request timeout.

## Transport

The transport is **io_uring**. It uses multishot accept/recv over a shared
provided-buffer pool, registered (fixed) file descriptors, and compile-time-asserted
ABI structs. `build.sh` produces one binary, `bin/server`, from `src/server.nim`,
and `start.sh` execs it directly. If `io_uring_setup` (or the fixed-file
registration) is refused at run time, the process logs a clear message and exits
non-zero.

## Why io_uring

io_uring keeps CPU low at high user counts with tight p50/p95/p99 tails, and it is
what the official environment runs. Registered fds remove the process
`RLIMIT_NOFILE` wall for held connections, and a shared provided-buffer pool keeps
per-connection memory tiny.
Per-feature failures short of a missing ring (a rejected setup flag, no multishot
recv, no multishot/direct accept, no buf ring) degrade in place without a restart.

## SQLite integration

- Vendored amalgamation, compiled in-tree (`vendor/sqlite3.c`), so there is no
  system `libsqlite3` dependency.
- One connection, `SQLITE_OPEN_READWRITE|NOMUTEX`, statements prepared once with
  `SQLITE_PREPARE_PERSISTENT`.
- Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `busy_timeout=5000`,
  `foreign_keys=ON`, `temp_store=MEMORY`, `cache_size=-1000`,
  `mmap_size=1 GiB` (`SQLITE_MMAP_SIZE`), `wal_autocheckpoint=0`,
  `locking_mode=EXCLUSIVE`.
- WAL checkpointing is explicit and off the request path: autocheckpoint is off,
  and a PASSIVE checkpoint runs at most once a second from the loop after a write.
- Durability: a write is committed before its response is sent; `NORMAL` survives
  a process crash and never corrupts the file.

## Robustness and bounds

See the "Robustness & bounds" section of the [README](../README.md). In short:
no request can abort the process (all handler exceptions, including `Defect`s,
become `500`), framing is strict, input and output are bounded, and parsing is
O(n).
