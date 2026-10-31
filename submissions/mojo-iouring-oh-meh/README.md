# Mojo + io_uring + SQLite B-tree, direct on port 80

A feed server in [Mojo](https://www.modular.com/mojo) 1.2, the Python-syntax systems
language whose compiler went open source in August 2026. Everything above the syscalls
is Mojo: the event loop, HTTP/1.1 parsing, JWT verification with its own SHA-256, JSON
validation, SQLite record decoding, and the query plans that walk SQLite's B-tree pages
directly. A 400-line C shim holds the plumbing Mojo can't express yet (liburing's inline
prep helpers, SQLite's file-private B-tree entry points, a checkpoint thread).

| | |
|---|---|
| Language | Mojo 1.2.0 (pinned PyPI wheel, 40 MB), `mojo build` at -O3 |
| Framework | none |
| Transport | io_uring (liburing 2.15): multishot accept into direct descriptors, multishot recv from provided buffers, one send in flight per connection |
| Database | SQLite 3.54.1 amalgamation compiled into the shim, `THREADSAFE=2`, `unix-excl` VFS, 32 MB page cache, no mmap |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the box) |
| Threads | one event loop, plus a C thread that runs passive WAL checkpoints |
| Kernel | needs Linux 6.1+ (multishot accept/recv); tested on Ubuntu 24.04's 6.8 |

## Why Mojo

The other 150 entries settle the "which language" question: the top sixteen are the same
design in C, Zig, Rust, Lean-generated C, Crystal, Ada, assembly and Objective-C, all
within 15 % of each other, because three quarters of each request is the kernel's TCP
path. What's left to learn is whether a three-month-old language with Python's surface
can hold that design without a C core. This entry is the experiment: the hot path is Mojo,
measured, with the shim kept to plumbing so the question stays honest.

## The design

**Connections.** Accepted sockets become io_uring direct descriptors, so `LimitNOFILE=65535`
caps a ring's table, not the server: rings are opened lazily, each with a table of
`RLIMIT_NOFILE - 64` slots. Accepts are one-shot direct accepts, up to 64 outstanding and
never more than the current ring has free slots, moving to the next ring when one fills;
multishot accept was tried first and loses one connection per ring to `ENFILE` when a
burst overruns the table. One thread drives all rings: with one ring live it blocks in a single
`io_uring_enter` that submits and waits; with several it sleeps in `epoll_wait` on each
ring's registered eventfd (a `DEFER_TASKRUN` ring signals it when work arrives) and runs
each signalled ring's task work. Idle connections are never closed and own no buffer:
receive buffers come from a provided-buffer pool per ring (2,048 × 2 KB), returned to the
kernel in contiguous runs once per batch. Per-connection state is a few words in
`calloc`ed tables, so untouched slots cost nothing.

**A batch is one wake-up.** Every completion is handled: requests are parsed in place
in the receive buffer, reads are answered immediately from the batch's SQLite
transaction (one `BEGIN` per batch, B-tree cursors opened once per batch), writes are
queued. Then the cursors close, the writes run as prepared statements, `COMMIT`, and only
then are the batch's sends submitted, so a 201 is always a committed row and reads never
see uncommitted ones. If the commit fails, every write of the batch answers 500.
Responses are written straight into an arena that stays alive until its last send
completes; constant error responses are built once at startup.

**Reads on the B-tree layer** (allowed per the organizer's ruling on the Lean entry).
`GET /feed` is the last 20 rows of the posts tree, newest first, plus one range scan of
`likes_post_id_idx` that tallies all 20 like counts, plus one rowid seek into `users` per
row. That plan is only correct when post ids are dense and `created_at` never decreases
with id (then `ORDER BY created_at DESC, id DESC` is reverse rowid order), so the server
checks both at startup with SQL and after every insert, and falls back to the
reference SQL query for good if either ever fails. `GET /posts/:id` is a rowid seek plus
the same two lookups. Every row is read from SQLite's pages inside the request's
transaction; nothing is remembered across requests. Record decoding (varints, serial
types) is in `src/record.mojo`.

**Writes** are the reference `INSERT ... RETURNING` and `INSERT ... ON CONFLICT DO
NOTHING`, group-committed per batch. WAL with `synchronous=NORMAL`. SQLite only restarts a
WAL on its own during a write lull, and group commit never leaves one, so the writer
runs a `TRUNCATE` checkpoint once the WAL passes 8,192 frames; the passive checkpoints on
the second thread (every 250 ms) keep that to a few milliseconds. Without it the WAL grew without
bound and `walFindFrame` was a quarter of all CPU.

**Auth.** HS256 only. HMAC-SHA256 is in Mojo with the key's inner and outer pad states
hashed once at startup; every token's signature is verified on every request (rule 5).
`exp` is required and compared to the batch's clock; `sub` must be a positive integer
string; `username` is taken as-is.

**HTTP.** Request line, `Authorization`, `Content-Length`, `Connection`; everything else
is skipped. Requests that span receives are completed in a per-connection buffer (16 KB
cap, 413 beyond it); pipelined requests are answered in order with one send in flight.
HTTP/1.0 or `Connection: close` closes after the response.

**Memory.** The point of the design: the leaders stall near 290,000 users because the
kernel's ~4 KB per idle socket plus their 800–1,400 MB of app memory fills the 2 GB
box before CPU runs out. This server's own footprint is the SQLite page cache (32 MB),
the per-ring buffers (4 MB each), response arenas (1 MB each, a handful in flight) and
a few words per connection. The startup invariant scan runs on a throwaway connection
without mmap and the file's pages are dropped from the page cache afterwards, so the
server starts at about 10 MB rather than with the whole posts table charged to it. A
guard pauses accepting when `MemAvailable` drops below
160 MB and resumes above 256 MB, so running out of memory becomes a failed probe
rather than a frozen box.

## What was measured

**x86, the benchmark's shape.** A DigitalOcean 1 vCPU / 2 GB Premium Intel droplet
(2.0 GHz, 1,967 MB, Ubuntu 24.04, kernel 6.8.0-147), the server started by systemd as
user `deploy` with `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE` on `0.0.0.0:80`, load
from a 4 vCPU droplet in the same datacenter over the private network. That load box
caps what k6 can drive (about 16,000 req/s and 20,000 users), so **no user-count score
is claimed**; the numbers below are what the organizer's search will be working with.

| Run (x86 droplet) | Result |
|---|---|
| `install.sh` / seed / `test/run.sh` | 35 s / 31 s / 64 s (build plus 42/42 checks), 369 MB compile peak |
| user model without think time, 500 connections, 30 s | 16,000 req/s (k6-bound), server at 45 % CPU: **28 µs of CPU per request, kernel included**; server-side feed latency during it: median 1.2 ms, p90 3.8 ms |
| `bench/load.js`, 20,000 users, 60 s ramp + 120 s | 0 errors, p99 4.4 ms (k6's view), server-side probe median 0.8 ms; **app cgroup peak 132 MB**; kernel slab +83 MB, i.e. ~4.2 KB of kernel memory per idle connection |
| idle | 34 MB cgroup, 11 MB RSS |

Profile at saturation (perf, 15 s): 65 % kernel (TCP, io_uring, the virtio notify
`iowrite16` alone is 10 %), 31 % server, 3 % libc. The largest user-side items are
HTTP parsing, `sqlite3BtreeTableMoveto` for the users rows, record decoding and
`pcache1Fetch`. The first profile had `walFindFrame` at 12 %: with `mmap_size` set,
SQLite bypasses its page cache for mapped pages and consults the WAL index on every
page touch, so mmap is off and the page cache is 32 MB instead.

What that implies on the benchmark box (same RAM, a faster Xeon): at 28 µs/request the
CPU runs out near 35,000 req/s, which is roughly 340,000 users in the load model, and
memory at ~4.2 KB per connection in the kernel plus ~150 MB of app leaves room for
about 380,000 connections in 1,967 MB. Both are estimates, not measurements.

**arm64 VM** (4 vCPU Ubuntu 24.04, k6 over loopback), for development:

| Run | Result |
|---|---|
| `test/run.sh` | 42/42 |
| `bench/load.js`, 2,000 and 10,000 users | 0 errors, p99 0.6 ms |
| user model without think time, 500 connections, one ring | 79,600 req/s, 0 errors, 5.2 µs of server CPU per request |
| same, forced onto 64-slot rings (8 rings in use) | 77,300 req/s, 0 errors, 5.6 µs/request |

## Build

`install.sh` (root) installs the C toolchain and Mojo 1.2.0 into `/opt/mojo` via a Python
venv. `build.sh` (user) downloads the SQLite amalgamation and liburing (sha256-pinned),
compiles the shim with the amalgamation into a static library, and runs `mojo build`
against it. `start.sh` runs `bin/server`, configured only by the SPEC.md variables.
The Mojo compiler peaks around 300 MB of RAM on this program; the shim compile around
370 MB.

## Honesty notes

- Ubuntu's 6.8 kernels reject `IORING_REGISTER_PBUF_RING`, so provided buffers use
  the classic `IORING_OP_PROVIDE_BUFFERS`, re-provisioned in runs with the completion
  skipped.
- Multiple rings were exercised only with small tables in a test; the 65k-slot layout
  follows from the kernel's `RLIMIT_NOFILE` check in `io_sqe_files_register`.
- The Mojo runtime starts a few idle worker threads at startup; the server never
  schedules work on them.
- The B-tree calls go through SQLite's own pager, WAL and locking, inside a SQLite
  transaction. They skip the bytecode engine, nothing else.

## Layout

```
src/server.mojo   event loop, rings, connection table, batches, routing
src/db.mojo       transactions, feed/post plans on B-tree cursors, writes
src/http.mojo     request parser
src/jwt.mojo      base64url, HS256 verification
src/sha256.mojo   SHA-256 and HMAC
src/jsonscan.mojo JSON validation and the "body" extraction; JWT payload fields
src/record.mojo   SQLite record format
src/resp.mojo     response arenas and headers
src/bytes.mojo    raw byte helpers, the writer, JSON escaping
src/uring.mojo    typed wrappers over the shim
src/shim.c        io_uring plumbing, SQLite B-tree exports, checkpoint thread
```
