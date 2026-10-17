# Nim: direct HTTP/1.1 on SQLite (the $12 Server Challenge)

A small Twitter-style API (feed, posts, likes) implemented in **Nim 2.2.12**, built from source,
served **directly** on `:80` with a hand-written, single-threaded **io_uring** event loop, over an
in-process **SQLite** database.

```bash
sudo bash install.sh                     # root: pinned Nim toolchain -> /opt/nim
bash build.sh                            # user: compile bin/server
SQLITE_PATH=/path/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

Environment: `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT` (defaults `127.0.0.1:3000`). The server is
healthy well within the 60 s the rules allow.

## Documentation

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**: the event loop, modules, request lifecycle, invariants and connection limits.
- **[docs/PERFORMANCE.md](docs/PERFORMANCE.md)**: what was measured, how to reproduce it, and the honest limitations.
- **[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)**: every runtime and build dependency, where it comes from, and how it is pinned.

## Transport

**Transport: io_uring.** `build.sh` produces a single binary, `bin/server`, from
`src/server.nim`. The HTTP parsing and response framing live in `src/http.nim`.

| Transport | How it is selected | What it adds |
|---|---|---|
| **io_uring** | the build; nothing to select | one thread, completion-queue driven; provided-buffer receive pool; a fixed-file table sized at startup from `RLIMIT_NOFILE`, so accepted sockets hold no new process fd (the table itself is charged to the limit once, at registration) |

```bash
bash build.sh                            # builds the single bin/server
bash start.sh                            # execs bin/server
```

`start.sh` execs `bin/server` directly. If the `io_uring_setup` syscall (or the fixed-file
registration) is refused at run time - `seccomp` in a container, `kernel.io_uring_disabled`, no
io_uring in the kernel - the process logs a clear message and exits non-zero.

## Why io_uring

io_uring wins on CPU and latency and it is the transport the official environment runs. Two
structural features matter here:

- **registered (fixed) file descriptors** - accepted connections live in the ring's fixed-file table
  and hold no new process `RLIMIT_NOFILE` slot (the table is sized at startup and charged to the
  limit once, at registration);
- **multishot accept/recv over a shared provided-buffer pool** - idle connections hold no recv
  buffer, so per-connection memory stays tiny.

It keeps CPU low at high user counts with tight p50/p95/p99 tails. Per-feature failures short of a
missing ring (a rejected setup flag, no multishot recv, no multishot/direct accept, no buf ring)
degrade in place without a restart.

## Toggles

Every switch, with its default. When nothing is set, the safe defaults are used.

Build time (`build.sh`):

| Variable | Default | Effect |
|---|---|---|
| `PGO` | `1` (on) | `PGO=0` builds without PGO: faster and deterministic, but ~16-20% more CPU per request |
| `PGO_TRAIN_ITERS` | `60` | PGO training iterations (larger = more representative profile, slower build) |
| `PGO_TRAIN_PORT` | `34999` | TCP port the PGO training server binds on `127.0.0.1` |
| `NIM_JOBS` | `0` (auto) | value passed to `nim c --parallelBuild`; `0` auto-detects the core count |

Run time (environment for `start.sh`):

| Variable | Default | Effect |
|---|---|---|
| `IOURING_RECV` | `multishot` | `oneshot` re-arms a one-shot recv (the p99 escape hatch) |
| `IOURING_ACCEPT` | `multishot` | `oneshot` re-arms a one-shot accept |
| `IOURING_ACCEPT_DIRECT` | `1` (on) | `0` forces the register-per-accept path instead of direct fixed-slot accept |
| `IOURING_BUFS` | `64` | provided-buffer pool size; rounded down to a power of two, clamped to `[64, 8192]` |
| `IOURING_BUFRING` | `1` (on) | `0` forces the `IORING_OP_PROVIDE_BUFFERS` list instead of the registered buf ring |
| `GROUP_COMMIT` | `1` (on) | `0` disables write group commit (per-request autocommit) |
| `TCP_DEFER_ACCEPT_SEC` | `1` | Seconds the kernel waits for the first client byte before accepting; `0` disables |
| `SQLITE_MMAP_SIZE` | `1073741824` | SQLite `mmap_size` in bytes (1 GiB); `0` disables mmap |
| `TCP_NOTSENT_LOWAT_BYTES` | `0` (off) | Cap the unsent write queue per connection (e.g. `131072`); this forces the register-per-accept path |
| `TCP_QUICKACK` | `0` (off) | `1` sets `TCP_QUICKACK` on accepted sockets; this forces the register-per-accept path |

Configuration: `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT` (default `127.0.0.1:3000`).

Everything else is deliberately fixed in code and not tunable at run time: one thread per process,
persistent prepared statements, WAL + `synchronous=NORMAL`, the 4096 listen backlog, EMFILE backoff,
the 1 s idle/stall sweep (75 s idle / 30 s stalled), the O(n) parser, and off-path WAL checkpoints.

## Architecture

One process, **one thread**, one event loop, io_uring. On a 1 vCPU box more threads only add context
switches; one loop keeps latency predictable and memory per connection tiny.

```
      io_uring completion queue
               │
      ┌────────┴────────┐
 listen fd         client fd
 multishot accept  recv / send
 (registered fds)  (buf pool)
                   │
                   ▼
      http.nim  HTTP/1.1 parse + response framing
                   │
                   ▼
      api.nim   routing · auth-before-id · exact errors
        │   │
 jwt.nim│   │jsonbuf.nim  byte-level JSON writer
        ▼   ▼
      db.nim ── sqlite3.nim (raw C API) ── in-process SQLite (WAL)
              │
              ▼
      one write per response  (backpressure / registered-fd send)
```

| File | Responsibility |
|---|---|
| `src/server.nim` | the io_uring loop: multishot accept/recv, provided-buffer pool, registered fds, group commit; also the socket scaffold (`raiseNofile`, `setupListenSocket`, `processRequests`) |
| `src/http.nim` | transport-agnostic HTTP/1.1 request parsing + response framing |
| `src/iouring.nim` | self-contained raw io_uring bindings (no liburing) |
| `src/api.nim` | routing + the five endpoints + exact error semantics |
| `src/db.nim` | all SQL; prepared statements; pragmas; durability ordering |
| `src/sqlite3.nim` | minimal raw `{.importc.}` bindings to the vendored SQLite C API |
| `src/jwt.nim` | base64url + HS256 HMAC verification (nimcrypto), constant-time compare |
| `src/jsonbuf.nim` | byte-level JSON writing and minimal request-body parsing |
| `src/model.nim`, `src/config.nim` | shared types; env-only configuration |

**Request flow:** an io_uring completion signals a connection → drain the socket → `http.nim` parses
every complete pipelined request in place → `api.handleRequest` (auth is checked *before* any id
parsing; every read hits SQLite) → `http.nim` frames the response bytes into the connection's output
buffer → submit sends until the socket would block; anything left is drained on the next completion so
a slow reader can never stall the loop.

### Event-loop invariants
- **Read to EAGAIN.** Each recv keeps consuming until `EAGAIN`; multishot recv re-arms from a buffer
  pool, so no readiness edge is ever lost.
- **Never block.** `recv`/`send`/`accept` are submitted asynchronously and never waited on inline.
- **Backpressure.** Unflushed output stays buffered and its send is re-submitted until the socket
  accepts it, so an already-writable socket is never missed.
- **Resource guards.** 16 KiB max header block; 1 MiB max unparsed input; idle 75 s / stalled 30 s.
- **Sparse slot-indexed state.** Per-connection state is a small pooled `Conn` slot, indexed by the
  fixed-file slot the socket occupies.

## Performance optimizations

**Transport and architecture**
- **No reverse proxy.** Nginx costs 14-20% of the single core; serving HTTP directly avoids that overhead.
- **io_uring transport, one thread.** A single loop over one completion queue; no worker pool, no
  thread handoff, no per-request object graph. Registering fds, multishot accept/recv and a shared
  provided-buffer pool keep CPU low at high user counts with tight tail latency.
- **Runtime-sized fixed-file table.** At startup the table is sized from `RLIMIT_NOFILE` as
  `min(hard - reserve, CAP)` with `CAP = 65536` and a 64-fd reserve, so the process-fd wall does not
  cap held connections and the size is printed in the startup log rather than hard-coded. If the
  kernel refuses the top size, startup steps the table down to a floor instead of exiting.
- **CQ headroom and overflow handling.** `IORING_SETUP_CQSIZE` asks for 4x the SQ entries (16384 CQEs
  against 4096 SQEs) so a completion burst near the connection ceiling cannot stall the loop; the
  kernel's reported `cq_entries` is logged. If the CQ still fills, the loop uses
  `IORING_SQ_CQ_OVERFLOW` and re-enters non-blocking to move parked completions back.
- **Provided-buffer receive pool (64 x 8 KiB).** Multishot recv draws from one shared, lazily-faulted
  pool, so idle connections hold no recv buffer.
- **One write per response**, with backpressure (the send is re-submitted until the socket accepts)
  so a slow reader never stalls the loop. A 64 KiB output cap (`MaxOutBuf`) stops the parser once that
  much is buffered, so a deep pipeline cannot pin a large heap buffer per connection.
- **Adaptive completion wait.** A pass that drained a batch of completions waits under a short timeout
  so queued sends are flushed promptly; an empty pass returns to the blocking wait.
- **Listener-level TCP options.** `TCP_DEFER_ACCEPT` (1 s) and `TCP_NODELAY` are set once on the
  listener, so accepted sockets inherit them with no per-connection `setsockopt` and direct accept
  stays enabled.

**Data path**
- **One SQLite connection, statements prepared once.** Statements use `SQLITE_PREPARE_PERSISTENT`; SQL
  never re-parses, and `SQLITE_ENABLE_QPSG` keeps the plan stable if a statement is ever re-prepared.
- **Dense-range feed scan with a decline path.** When the top-20 feed ids fit a small dense id window,
  the feed is rebuilt from three cheap range lookups; otherwise the loop falls back to the original
  `ORDER BY created_at DESC, id DESC LIMIT 20` query, kept verbatim, so the output is byte-identical.
- **Zero-allocation JSON write path.** Response bytes are written straight into the connection's output
  buffer (escaped string runs bulk-copied, integers written digit by digit), with no intermediate
  per-element string.
- **mmap + a small page cache.** Reads come from the OS page cache (size via `SQLITE_MMAP_SIZE`,
  default 1 GiB); the SQLite cache stays small (`cache_size=-1000`, kept after measurement).
- **WAL checkpoint off the hot path.** Autocheckpoint is disabled (`wal_autocheckpoint=0`) and a
  **PASSIVE** checkpoint runs at most once a second from the idle branch of the loop, so no request
  pays a checkpoint fsync; `journal_size_limit` still bounds the `-wal` file.
- **Write group commit** (default on): writes issued during one event-loop pass share a single
  transaction that commits before any dependent response is sent (`synchronous=NORMAL`, the documented
  floor the rules allow).
- **Compact `Conn`.** Per-connection state is a pooled slot of 80 bytes (the boolean facts are packed
  into one `set[ConnFlag]`), so the slot array stays small.
- **O(n) request parsing**: the input buffer is walked with a cursor and compacted once per event, so a
  large pipelined buffer is parsed in O(n) rather than O(n^2).

**Build**
- **Profile-guided optimization (PGO)**: a two-pass build trained on a short synthetic workload, with a
  plain-build fallback when PGO is unavailable.
- **Native + LTO + SQLite trims**: `-O3 -march=native -fno-semantic-interposition -fno-plt
  --stacktrace:off` and `-d:lto`, with SQLite compiled `THREADSAFE=0` and trimmed (`USE_ALLOCA=1`,
  `LIKE_DOESNT_MATCH_BLOBS`, `OMIT_DECLTYPE`, `OMIT_AUTOINIT`, `OMIT_*`, `DQS=0`, JSON1). See [Build](#build).

**Capacity**
- **Large listen backlog (4096), EMFILE backoff** (the acceptor pauses and retries on the next tick),
  a runtime-sized registered-fd table, and a 1 s idle/stall sweep (see
  [Peak connections](#peak-connections-the-real-limit)).


## Peak connections (the real limit)

The score is the number of simultaneous keep-alive connections the server can hold with good tail
latency. Two kernel limits sit under that number, and only one of them is a process limit:

- **Process file descriptors.** The target box gives the service `LimitNOFILE=65535`, which cannot be
  raised from inside the process. Registering the fixed-file table charges its entries against
  `RLIMIT_NOFILE`, so the table is sized `min(hard - 64, 65536)` and accepted sockets then hold no new
  process fd: the per-connection fd wall is gone, but the table size is still bounded by the limit.
- **Kernel connection tracking (conntrack).** The droplet's `nf_conntrack_max` is about 64k on 2 GB.
  That table is machine-wide and is **not** lifted by registered fds or by anything the server can do
  in-process. It is the practical ceiling for simultaneous established connections as seen from the
  target environment.

The loop keeps per-connection cost low so that this ceiling, not memory or CPU, binds first: a shared
provided-buffer receive pool (64 buffers of 8 KiB, so idle connections hold no recv buffer), an
80-byte `Conn` slot, and a compact runtime-sized slot array. The 4096 listen backlog absorbs reconnect
bursts and EMFILE backoff pauses the acceptor (it never spins); a 1 s sweep closes connections that
have been idle for 75 s or stalled for 30 s, which also keeps the open-connection count bounded over a
long run.

**Honesty about the numbers.** The ~64k conntrack ceiling is argued from the target environment
(`LimitNOFILE=65535`, a 2 GB droplet), not measured here. Local load tests run over **loopback**,
where the kernel's conntrack table is not engaged, so they exercise the socket and memory path but
**not** the conntrack path. A loopback run can therefore hold far more connections than the target box
ever will, and that is a property of loopback, not a promise about the droplet. What the server
controls is a low, bounded per-connection cost and a fixed-file table that keeps held
connections independent of the process fd limit.

This is a general capacity policy (what Nginx does when it runs out of connections), not benchmark
detection.


## Robustness & bounds

The server is built so that **no request can crash it** and every per-event operation is bounded:

- **No request aborts the process.** The request handler is wrapped so that any exception, including
  `Defect`s such as integer-overflow from hostile input (e.g. `GET /posts/99999999999999999999999`),
  is answered with `500` instead of propagating. Request ids are range-checked *before* the multiply,
  so over-long ids return `400`. (An earlier revision could be killed by one crafted id; this is fixed.)
- **Strict framing.** `Content-Length` must be all digits (no sign, no empty, no conflicting duplicate);
  `Transfer-Encoding` is rejected. Bad framing → `400` + close, never a body re-parsed as a request.
- **Bounded input.** Buffered *unparsed* input per connection is capped (about 1 MiB plus the header
  block), and every complete request in the buffer is processed each event. A client that pipelines
  more than the cap in a single burst is served up to the cap and then disconnected. Real clients,
  including the benchmark's `load.js`, send one request at a time, so this bound is never reached in
  the scored workload.
- **Bounded output / backpressure.** A response that cannot be sent in full stays buffered and its
  send is re-submitted; a slow reader can never stall the loop or grow memory without bound.
- **Timeouts.** Idle keep-alive 75 s; stalled mid-request 30 s.
- **Linear parsing.** Each event compacts the consumed input prefix once (no per-request front-delete),
  so a large pipelined buffer parses in O(n) rather than O(n²).

## Durability

`WAL` + `synchronous=NORMAL`. A `201` means the row is committed before the response is written.
`NORMAL` survives a process crash; on power loss the newest committed transactions may be lost while
the database stays uncorrupted. `synchronous=OFF` (which can corrupt the file) is not used.

## Build

- **Toolchain:** pinned Nim `2.2.12` binary tarball (sha256-verified in `install.sh`).
- **One binary:** `build.sh` builds `bin/server` from `src/server.nim`. PGO is preferred; a failed PGO
  pass falls back to the plain build, and a genuine compile failure is fatal.
- **SQLite:** the `3.54.0` amalgamation is downloaded by `build.sh` (sha256-verified, `curl --retry 3`)
  and compiled in-tree, so the submission has **no system `libsqlite3` dependency** and builds cleanly
  from source.
- **nimcrypto:** `src/jwt.nim` needs it for HMAC-SHA256. It is vendored the same way (pinned `0.7.3`
  tarball, sha256-verified, extracted into `vendor/nimcrypto`) and `config.nims` adds its path to every
  compile, so the build never depends on anything installed in a user Nimble directory.
- **Nim flags:** `--threads:off --mm:arc -d:release -d:lto --stacktrace:off` plus
  `--parallelBuild:${NIM_JOBS:-0}` (0 auto-detects the core count), plus
  `--passC:"-O3 -march=native -fno-semantic-interposition -fno-plt"` and `--passL:"-Wl,-O1"`.
  Runtime checks are intentionally **kept on** (`-d:release`, never `-d:danger`): a crash during the
  benchmark must never happen, and the check cost is negligible next to the syscall and SQL work.
- **SQLite trims:** `-DSQLITE_THREADSAFE=0` (the server is single-threaded), `-DSQLITE_DQS=0`,
  `-DSQLITE_DEFAULT_MEMSTATUS=0`, `-DSQLITE_USE_ALLOCA=1`, `-DSQLITE_LIKE_DOESNT_MATCH_BLOBS`,
  `-DSQLITE_OMIT_DECLTYPE`, `-DSQLITE_OMIT_AUTOINIT` (with one `sqlite3_initialize()` in `openDb`),
  `-DSQLITE_OMIT_DEPRECATED`, `-DSQLITE_OMIT_LOAD_EXTENSION`, `-DSQLITE_OMIT_PROGRESS_CALLBACK`,
  `-DSQLITE_OMIT_SHARED_CACHE`, `-DSQLITE_MAX_EXPR_DEPTH=0`, `-DSQLITE_TEMP_STORE=3`, `-DSQLITE_ENABLE_JSON1`,
  `-DSQLITE_ENABLE_QPSG`.

## Testing

- `bash test/run.sh submissions/nim-io_uring-NioHast`: the official 42-check suite, byte-exact. It
  builds `bin/server` and runs it via `start.sh`.
- Unit tests under `tests/`: `test_db.nim` (queries vs the seed DB and the `sqlite3` CLI),
  `test_jwt.nim` (HS256 against `openssl` vectors), `test_iouring.nim` (io_uring ABI + SQE/CQE round-trip).

## Development notes

Before writing the event loop the author **reverse-engineered `mummy`**, a fast,
multi-threaded Nim HTTP/1.1 server, to study how it wires an event loop, per-connection
buffers and keep-alive, then implemented this single-threaded server from scratch.
No `mummy` code is used, imported, or shipped.

## Compliance (challenge rules)

- **SQLite only**: every read/write is in-process SQLite (rule 2); no in-memory store, no external
  cache.
- **No caching across requests** (rule 5), no response cache; JSON and like counts are computed per
  request. Prepared statements and the SQLite page cache are allowed and used.
- **Durable before responding** (rule 6), commit before the response; no `synchronous=OFF`.
- **No hard-coded responses / no benchmark detection** (rule 7), everything derives from SQLite.
- **Nginx optional / direct** (rule 9), direct bind.
- **Built from source, pinned** (rule 10), pinned toolchain and pinned SQLite; nothing prebuilt is
  shipped; no system library beyond a C toolchain and libc is required.
- **No kernel tuning, no pinning** (rule 11), only in-process settings (socket options, SQLite
  pragmas, event loop, `setrlimit` of our own soft limit).

## Repository layout

```
submissions/nim-io_uring-NioHast/
  install.sh  build.sh  start.sh  README.md  .gitignore  config.nims
  docs/   ARCHITECTURE.md  PERFORMANCE.md  DEPENDENCIES.md
  src/    server.nim iouring.nim http.nim
          api.nim db.nim sqlite3.nim jwt.nim jsonbuf.nim model.nim config.nim
  tests/  test_db.nim test_jwt.nim test_iouring.nim
  tools/  pgo_train.sh pgo_flush.c gen_synth_db.c
  vendor/ (downloaded by build.sh; gitignored)   bin/ nimcache/ (gitignored)
```

## License

MIT, under the repository [license](../../LICENSE).
