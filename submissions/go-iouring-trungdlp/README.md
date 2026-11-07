# Go, io_uring, SQLite's B-tree cursors, one thread

One locked OS thread runs an io_uring event loop written directly against the kernel ABI, an
HTTP/1.1 parser, a JSON reader/writer and HS256 verification, all in Go and allocation-free on the
request path. SQLite (the pinned, unmodified amalgamation) is linked in through cgo, with one
call per request.

| | |
|---|---|
| Language | Go 1.27.3 (official tarball, sha256-checked by `install.sh`), cgo with gcc 13 for SQLite |
| Framework | none: own io_uring binding (raw syscalls, no liburing, no third-party Go modules), HTTP/1.1, JSON, HS256 (`crypto/sha256`) |
| SQLite | 3.54.1 amalgamation, sha256-checked, downloaded by `build.sh`, compiled with `-O3 -march=native` (no stack protector, CET or fortify checks); Go code built for x86-64-v3 when the CPU has it |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors) |

## Request path

- **Reads** (`GET /feed`, `GET /posts/:id`) walk SQLite's own B-tree cursors in `shim.c`, inside the
  pass's transaction: the feed index (`posts_created_at_id_idx`, already in
  `created_at DESC, id DESC` order) for the 20 newest ids, the `posts` rows in ascending id order
  (consecutive ids are one step along a leaf), the authors in `users`, and the like counts with one
  range walk of `likes_post_id_idx`. Only SQLite's B-tree API is used to read (`TableMoveto`,
  `IndexMoveto`, `First`/`Next`, `PayloadSize`/`PayloadFetch`); nothing in SQLite's state is
  written. Column values are copied out of the pages into a result struct that Go turns into JSON.
  The reference SQL answers if the data isn't in the form the plan expects (a post without a user,
  an index row without its post), and the server refuses to start on a different schema.
- **Writes** go through prepared SQL: `INSERT … RETURNING id, created_at` and
  `INSERT … ON CONFLICT DO NOTHING` (`sqlite3_changes` tells a new like from a repeat).
- **One call into C per request** (`td_feed`, `td_post`, `td_like`, `td_insert_post`) plus one
  `COMMIT` per pass, made through a small assembly trampoline rather than cgo's call path (see
  Go runtime).
- **JWT:** the HMAC key's inner and outer blocks are hashed once at startup and their SHA-256
  states restored per request, so each verification hashes only `header.payload` and the outer
  digest. Signature (canonical base64url only), `alg` (HS256 only) and `exp` (required) are checked
  on every request (rule 5).
- **JSON output** for a feed or a post is written by `shim.c` straight into the response buffer
  from the columns the read copied out: each text is checked for characters to escape 32 bytes at
  a time (AVX2) while it is copied, so the usual text is one pass. (Rendering in Go cost about 3%
  more CPU per request.) Content-Length is a fixed 5-character field filled in after the body,
  right-aligned (leading spaces are optional whitespace, RFC 9110 5.6.3), so no body is ever moved.
  [bytedance/sonic](https://github.com/bytedance/sonic) (`ConfigFastest`) was measured on the same
  20-post feed: 2 to 4 times slower than the hand-written Go encoder, and it allocates.
- **JSON input** is validated in full (malformed → 400); the last duplicate key wins, as in
  `JSON.parse`. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code
  points.

## Durability and batching

- WAL with `synchronous=NORMAL`, `unix-excl` VFS (one process owns the file; the WAL index lives in
  heap memory, no `-shm` file), 96 MB page cache, no mmap, automatic checkpoints off.
- SQLite's own page cache sits behind a thin `SQLITE_CONFIG_PCACHE2` wrapper that skips a commit's
  truncate when no page past the end of the file can be cached. pcache1 never lowers the largest
  page number it has held, and a B-tree split briefly parks a page at the lock-byte page's number,
  so afterwards every commit would scan the whole cache's hash table to drop nothing (0.5 to 1% of
  the CPU in profiles, growing as the cache fills). The idea is c-iouring-pavlyhalim's (#245).
- **Each event-loop pass is one SQLite transaction.** Its reads share a snapshot and its writes are
  group-committed (rule 6). No response from a pass reaches the kernel before its `COMMIT` returns;
  if a commit fails, every response the pass produced becomes a 500 and the connection closes.
- **A pass waits up to 1 ms** (or for 256 completions) before answering, as #264 does. On x86,
  1 ms, 2 ms and 4 ms windows measured the same CPU per request within noise (41.0, 41.1 and
  41.7 µs, three runs each); the shorter window keeps the bursts of sends after each `COMMIT` small.
- Checkpoints run on a second thread at nice 19 on its own connection (PASSIVE), started past
  4,096 WAL frames; while it copies the log's tail, writes are held between passes for at most
  250 ms so the log can restart. This is rust-iouring-kensac's (#264) scheme; see Credits.

## Connections

- Accepted sockets are io_uring **direct descriptors** (fixed-file table, not the process fd table).
  A table holds at most `RLIMIT_NOFILE` (65,535) slots, so the server adds rings as they fill (up
  to 16), accepting on the next ring while a margin of slots is still free. Eight `SO_REUSEPORT`
  listeners give eight accept queues of `somaxconn` each.
- One thread drives every ring (`SINGLE_ISSUER`, `DEFER_TASKRUN`). **Every non-waiting
  `io_uring_enter` asks for many completions with a zero timeout** instead of none: newer kernels
  run at most `max(min_complete, 20)` deferred completions per enter, so with `min_complete = 0`
  each extra ring would see only ~20 completions per pass. With four rings at 240k connections
  that alone turned a passing run into one with 5-second latencies.
- Each connection has one **multishot receive** into a per-ring provided-buffer ring (1,024 × 2 KB),
  so an idle connection holds no receive buffer. A receive that ends (ENOBUFS after a burst) is not
  re-armed at once but read with bounded single-shot `MSG_DONTWAIT` reads, then re-armed. A pass
  answers at most 2,048 requests; the rest wait on their connection, oldest first.
- The connection table holds no Go pointers (buffers are pool indexes), so the collector never
  scans it. Responses use 8 KB pooled buffers; bytes read but not yet answered use small (512 B)
  ones, so a backlog of tens of thousands of waiting requests costs megabytes. Idle connections are
  never closed by the server.

## Go runtime

- `GOMAXPROCS=1`, `runtime.LockOSThread`, garbage collection off (`SetGCPercent(-1)`) with a
  512 MB memory limit as a backstop; the request path allocates nothing.
- Every `io_uring_enter` is a raw syscall, the 1 ms wait included: as an ordinary blocking syscall
  the runtime's sysmon thread would take the P away on every wait and then poll every 20 µs.
- **C is called without cgo's call path** (`ccall/`, `ccall_amd64.go`). A cgo call marks the
  thread as in a system call, and with one P, sysmon takes the P back from any call it finds still
  running on its next check, drops its own polling interval back to 20 µs, and the loop must win
  the P back when C returns. At moderate load that is cheap; near saturation, when interrupts
  stretch C calls, it fed on itself: in a 1 vCPU VM at the edge, sysmon used 13% of the CPU and
  preempted the loop 2,000 times a second. Here an assembly trampoline switches to a stack of
  its own (8 MB, `mmap`ed, guard page below) and calls the C function with the System V
  convention, so the goroutine never leaves the running state. Only the loop's goroutine calls
  in, and nothing in `shim.c` calls back into Go. (Elsewhere than amd64, plain cgo calls.)
- `GODEBUG=asyncpreemptoff=1`: no preemption signals interrupting the loop.

## Measured

**On the benchmark box:** #339 (`e78b5a4`) held 260,000 users; #343 (`39efd02`) held 270,000 and
failed 280,000 on p99 (2.7 s) at 86% CPU with 0 errors.

**At the edge, in a box-shaped VM:** a KVM guest with 1 vCPU and 2 GB (1,967 MB visible, as on the
box), Ubuntu 24.04's cloud image with its 6.8 kernel, a virtio NIC, the server as a systemd
service with `LimitNOFILE=65535`; the load generator (a Go port of `bench/load.js`) on the host.
The guest runs nested on a Haswell VPS, so everything costs about three times what it does on the
box and the edge sits near 110,000 users, but it fails the way the box does: softirq work and the
loop compete for the one CPU, and a burst at the end of the ramp or a checkpoint tips it over.
Probes are 60 s ramp + 60 s hold, builds alternating:

| users | #343 | this version | rust-iouring-kensac (#264) |
|---:|---|---|---|
| 90,000 | p99 2.4 s, 2.5 s, 0.4 s (1 of 3 pass) | | p99 0.4, 0.8, 0.6 s (3 of 3) |
| 95,000 | | p99 0.31, 0.31, 0.76 s (3 of 3) | p99 0.41, 0.61, 0.35 s (3 of 3) |
| 105,000 | p99 3.8 s, 0.7 s (1 of 2) | p99 0.56, 0.49 s (2 of 2) | p99 0.28, 0.43 s (2 of 2) |
| 110,000 | | p99 0.57, 1.8, 0.34 s (2 of 3) | p99 1.9, 2.6, 1.8 s (0 of 3) |

The difference between #343 and this version is the C call path (see Go runtime): in the failing
#343 runs the guest's CPU went to 90%+ with sysmon at 13% of it.

**This version, on x86:** a Haswell VPS (Ubuntu 24.04, kernel 6.8), the server alone on one
reserved core, the load generator (a Go port of `bench/load.js`, same user model) on the others in
a separate network namespace, connected by a veth pair whose receive processing is steered to the
server's core (so that core also does the packet work a NIC box's only core does). CPU is that
core's total busy time during the hold, per request; runs alternate between builds.

| 60,000 users, 60 s hold | µs per request (each run) | mean |
|---|---|---:|
| this version | 39.1, 39.4, 39.9 | 39.4 |
| rust-iouring-kensac (#264) | 41.1, 39.5, 41.1, 40.4 | 40.5 |
| #339 as scored | 42.1, 40.0, 41.9, 43.8 | 42.0 |

Per request the kernel's TCP work is about two thirds of the CPU and the same for every entry
(2.2 packets in and 1.3 out per request here, for this entry and #264 alike); the differences
are in user space. The steps, each measured in alternating runs:
`GOAMD64=v3` and SQLite without hardening flags −4%, JSON rendered in C −3%.

## Credits

The overall design (one io_uring thread, direct descriptors over several rings, SQLite B-tree reads,
group commit per pass, bounded reads after a burst) follows what
[rust-iouring-kensac](../rust-iouring-kensac) (#264) and [c-iouring-pavlyhalim](../c-iouring-pavlyhalim)
(#245) describe in their READMEs. In `shim.c`, the checkpoint thread and its hold state machine
(`ck_main`, `td_tick`) and the index seek helper (`seek_ge`) closely follow #264's `csrc/shim.c`
(credited reuse, per Arjay's 2026-11-04 ruling). The page cache wrapper (`pcw_*`) implements
#245's idea of skipping the commit-time truncate scan. Everything else (the Go io_uring binding, event
loop, HTTP, JSON, JWT, the feed/post read plans and record decoding in `shim.c`) is written from
scratch.

## Files

| | |
|---|---|
| `main.go` | environment, listeners, runtime settings |
| `server.go` | the event loop: rings, accept/receive/send/close, passes, buffers |
| `uring.go` | io_uring setup, submission/completion rings, operations, provided buffers |
| `app.go` | routes, handlers, responses |
| `http.go`, `json.go`, `jwt.go` | request framing, JSON in and out, HS256 |
| `db.go`, `shim.c`, `shim.h` | bindings and the SQLite layer (B-tree reads, SQL writes, checkpoints) |
| `ccall_amd64.go`, `ccall/` | calls into `shim.c` through an assembly trampoline (`ccall_other.go`: plain cgo elsewhere) |

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
