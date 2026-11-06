# Go, io_uring, SQLite's B-tree cursors, one thread

One locked OS thread runs an io_uring event loop written directly against the kernel ABI, an
HTTP/1.1 parser, a JSON reader/writer and HS256 verification, all in Go and allocation-free on the
request path. SQLite (the pinned, unmodified amalgamation) is linked in through cgo, with one cgo
call per request.

| | |
|---|---|
| Language | Go 1.27.3 (official tarball, sha256-checked by `install.sh`), cgo with gcc 13 for SQLite |
| Framework | none: own io_uring binding (raw syscalls, no liburing, no third-party Go modules), HTTP/1.1, JSON, HS256 (`crypto/sha256`) |
| SQLite | 3.54.1 amalgamation, sha256-checked, downloaded by `build.sh`, compiled with `-O3 -march=native` |
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
- **One cgo call per request** (`td_feed`, `td_post`, `td_like`, `td_insert_post`) plus one
  `COMMIT` per pass. Measured with `perf`, cgo's own overhead is about 0.4% of the CPU.
- **JWT:** the HMAC key's inner and outer blocks are hashed once at startup and their SHA-256
  states restored per request, so each verification hashes only `header.payload` and the outer
  digest. Signature (canonical base64url only), `alg` (HS256 only) and `exp` (required) are checked
  on every request (rule 5).
- **JSON output** is checked for characters to escape 8 bytes at a time (SWAR) and copied in one
  piece when there are none (the usual case). Content-Length is a fixed 5-character field filled in
  after the body, right-aligned (leading spaces are optional whitespace, RFC 9110 5.6.3), so no
  body is ever moved.
- **JSON input** is validated in full (malformed → 400); the last duplicate key wins, as in
  `JSON.parse`. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code
  points.

## Durability and batching

- WAL with `synchronous=NORMAL`, `unix-excl` VFS (one process owns the file; the WAL index lives in
  heap memory, no `-shm` file), 96 MB page cache, no mmap, automatic checkpoints off.
- **Each event-loop pass is one SQLite transaction.** Its reads share a snapshot and its writes are
  group-committed (rule 6). No response from a pass reaches the kernel before its `COMMIT` returns;
  if a commit fails, every response the pass produced becomes a 500 and the connection closes.
- **A pass waits up to 4 ms** (or for 4,096 completions) before answering. Measured at 40k req/s on
  one core, 1 ms / 2 ms / 4 ms windows cost 11.1 / 10.7 / 10.5 µs per request (three runs each):
  a longer window shares each `COMMIT`, its WAL frames and each `io_uring_enter` among more
  requests. Latency grows by a few milliseconds; 8 ms and 16 ms were worse again.
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
- Every `io_uring_enter` is a raw syscall, the 4 ms wait included: as an ordinary blocking syscall
  the runtime's sysmon thread would take the P away on every wait and then poll every 20 µs.
- `GODEBUG=asyncpreemptoff=1`: no preemption signals interrupting the loop.

## Measured (local, Docker on an Apple M4 Pro, server limited to 0.43 of one core and 1.7 GB)

The load generator (a Go port of `bench/load.js`, same user model) ran 60 s ramp + 120 s hold. Same
setup, same day, against rust-iouring-kensac (#264, #1 on the board when this was written):

| users | this entry | rust-iouring-kensac |
|---:|---|---|
| 220,000 | pass: p95 28 ms, p99 112 ms, 16.2 µs/req, 1.02 GB peak | fail: p99 1.7 s, 18.1 µs/req, 1.38 GB |
| 230,000 | pass: p95 124 ms, p99 229 ms, 16.9 µs/req, 1.03 GB peak | fail: p99 4.4 s, 18.2 µs/req, 1.47 GB |
| 240,000 | fail (p99 at the end of the ramp) | |

The absolute numbers depend on the CPU limit chosen to imitate the box; the comparison is the point.

## Credits

The overall design (one io_uring thread, direct descriptors over several rings, SQLite B-tree reads,
group commit per pass, bounded reads after a burst) follows what
[rust-iouring-kensac](../rust-iouring-kensac) (#264) and [c-iouring-pavlyhalim](../c-iouring-pavlyhalim)
(#245) describe in their READMEs. In `shim.c`, the checkpoint thread and its hold state machine
(`ck_main`, `td_tick`) and the index seek helper (`seek_ge`) closely follow #264's `csrc/shim.c`
(credited reuse, per Arjay's 2026-11-04 ruling). Everything else (the Go io_uring binding, event
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
| `db.go`, `shim.c`, `shim.h` | cgo bindings and the SQLite layer (B-tree reads, SQL writes, checkpoints) |

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
