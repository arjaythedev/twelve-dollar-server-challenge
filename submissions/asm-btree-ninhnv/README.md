# x86-64 assembly: io_uring + SQLite B-tree reads

A feed server whose application code is hand-written x86-64 assembly (NASM): the io_uring event
loop (raw `io_uring_setup`/`enter`/`register` syscalls on hand-mapped rings, no liburing), HTTP/1.1
framing, routing, a validating JSON reader, JSON output, SHA-256/HMAC-SHA256, base64url, the JWT
checks, and the decoding of SQLite's B-tree records. One thread drives everything except WAL
checkpoints, which two helper threads run.

The design follows [rust-iouring-kensac](../rust-iouring-kensac) (one pass = one SQLite
transaction, reads through SQLite's B-tree cursors, direct descriptors, multishot receives, a
checkpoint thread with write holds, drain-first recovery after a stall), re-implemented in
assembly.

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 `nasm`), System V ABI throughout |
| C | `src/shim.c` only: it `#include`s the SQLite amalgamation to reach B-tree functions that are `static` inside it (see below). Linked with `gcc` against glibc |
| SQLite | 3.54.1 amalgamation, unmodified, sha256-pinned in `build.sh`, compiled with `-O3 -march=native` |
| Dependencies | none besides SQLite and libc |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, sparse direct-descriptor tables). Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero (Launchpad #2162843), so a refused registration is retried once with `resv[0]=1` |

## Files

| | |
|---|---|
| `src/loop.asm` | `main`, ring setup, accept/receive/send/close, the read queue and ready list, the pass |
| `src/app.asm` | routing, the five handlers, response framing (`Content-Length` back-filled), JS `trim()` |
| `src/db.asm` | record decoding, the feed and single-post plans, the likes-index walk |
| `src/jwt.asm` | HS256 verification, `sub`/`exp`/`username` checks |
| `src/sha256.asm` | SHA-256 compression, HMAC with precomputed key blocks, base64url decoding |
| `src/json.asm` | validating JSON reader (strict UTF-8, escapes, nesting) and unescaper |
| `src/http.asm` | request-line and header parsing (SSE2 byte search) |
| `src/out.asm` | integer and JSON-string writers, output-buffer pool, kept-input buffers |
| `src/shim.c` | the amalgamation plus thin wrappers: open, per-pass transaction, cursor moves, record bytes, a page view, the two INSERTs, `SELECT 1`, the SQL fallback, the checkpoint threads |
| `tools/lg.c` | the load generator used for the measurements below (not part of the build) |

## Reads go through SQLite's B-tree, not its SQL engine

The assembly asks the shim to move a cursor and gets back the record bytes SQLite holds for that
row. It then decodes the record header (varints and serial types) itself and writes the JSON
straight from those bytes. There is no bytecode engine and no `sqlite3_column_*` on the read path.

- **Feed:** walk `posts_created_at_id_idx` from its first entry. It is `(created_at DESC, id DESC)`,
  so that is exactly `ORDER BY created_at DESC, id DESC`, and its records carry `created_at` and
  `id`. Then the 20 posts are looked up in ascending id order: consecutive ids are the next cell
  of the same leaf (SQLite's own `TableMoveto` fast path). Then the 20 authors in `users`. Like
  counts come from one range walk of `likes_post_id_idx` when the 20 ids lie within 512 of each
  other: the assembly reads every cell of a leaf directly from a page view the shim exposes
  (`td_pv`) and moves to the next leaf through SQLite. Otherwise it seeks per post.
- **Nothing assumed about the data:** if the ids aren't in descending order they are sorted; if a
  post has no author row (the SQL `JOIN` would drop it) the reference SQL query answers instead; a
  likes cell in an unexpected form falls back to decoding entry by entry. At startup the shim
  checks the column order of every table and index the assembly decodes and refuses to start on
  any other schema.
- **Live reads (rule 5):** every response is built from rows read while it is served, inside the
  pass's transaction. Only prepared statements, cursors, two KeyInfo objects and the four root page
  numbers (read from `sqlite_schema` at startup) are reused. Every JWT signature is verified on
  every request.
- **Writes use SQL:** `INSERT INTO posts (user_id, body) …` and `INSERT … ON CONFLICT DO NOTHING`
  (`sqlite3_changes` tells a new like from a repeat). A new post's `created_at` (the column
  default) is read back from its row through the posts cursor, in the same transaction:
  `RETURNING` gives the same values but buffers them in a temporary B-tree, with its own pager and
  page cache, built and torn down on every call. The like handler's 404 check is a B-tree seek.

## Durability and batching

- WAL with `synchronous=NORMAL`, opened with SQLite's `unix-excl` VFS: the process keeps an
  exclusive lock on the file, so the WAL index lives in heap memory shared by its two connections
  (no `-shm` file, no file locks per transaction). 64 MB page cache, no mmap.
- **One pass = one transaction.** The first request in a pass that touches the database begins it,
  every read and write of the pass runs inside it, and the pass ends with one `COMMIT` (group
  commit, rule 6). **No response from a pass is handed to the kernel until that `COMMIT` returns.**
  If it fails, every response the pass produced is replaced by a 500 and the connection is closed.
- **Checkpoints off the request path.** Automatic checkpoints are off. Once the WAL holds 4,096
  frames, a helper thread (nice 19, own connection) runs a PASSIVE checkpoint. Then writes are
  briefly held: the next POST on each connection stays unread while a second helper thread copies
  the frames written meanwhile, so the next write can restart the log from the top. That one runs
  at normal priority because writes wait for it: at nice 19 it got ~1.5% of a busy CPU and holds
  ran into their limit (below). Reads go on during a hold, and a hold lasts at most 250 ms. Past
  65,536 frames the loop checkpoints by itself. In a
  30-second write-heavy run (27k posts and 200k likes at ~96k req/s) the WAL file never grew past
  33 MB and `integrity_check` was `ok` afterwards.
- **Page cache after a split.** When a write splits a B-tree page, SQLite renumbers the new pages
  through a temporary page number far past the end of the file, and the page cache keeps that as
  its largest key. The commit then truncates the cache to the file size, and from that key it
  walks every cached page (all of the 64 MB cache) rather than the few numbers past the end.
  Before each commit the shim checks that nothing is cached at that number and puts the largest
  key back at the file size, so the truncate has nothing to walk.
- **Batch size.** Each pass waits on ring 0 for 256 completions or 4 ms, whichever comes first
  (never while reads or answers are queued). Bigger passes share one `COMMIT` and fewer WAL frame
  writes. At ~18k req/s over 18,000 connections, 4 ms instead of 1 ms cut server CPU from 17.8 to
  16.0 µs per request, for about 2–3 ms more median latency.

## Connections

- Accepted sockets are io_uring **direct descriptors** (`IORING_FILE_INDEX_ALLOC` into a sparse
  table), not process descriptors. A table can't exceed `RLIMIT_NOFILE`, so the server raises its
  soft limit to the hard one and gives each ring that many slots. When a ring's table is within a
  margin of full (1/8 of it, at most 1,024), accepting moves to another ring, adding one (up to 16).
  Accepts are single-shot, at most 512 in flight, fewer than the margin, so an accept never lands
  on a full table and drops a connection. Eight `SO_REUSEPORT` listeners each get their own
  listen queue. Tested here with the descriptor limit lowered to 2,048: 7,000 connections over
  4 rings, no errors.
- Each connection has one **multishot receive** into a provided-buffer ring (1,024 × 2 KB per ring),
  so an idle connection holds no receive buffer. A connection record is 64 bytes; its output and
  kept-input buffers are null pointers while idle.
- **Bounded reads after a burst.** A receive that ends (buffers ran out, `ENOBUFS`) is not
  re-armed. The connection goes on a FIFO read queue, and each pass issues single-shot
  `MSG_DONTWAIT` receives for it, never more on a ring than it has buffers. Once a socket is drained
  its multishot receive is armed again.
- **Drain first, oldest first.** A pass reads up to 16,384 times but answers at most 2,048 reads.
  Bytes read beyond that are kept on their connection in a heap buffer sized to them, and the
  connection goes on a ready list that later passes answer in arrival order. While that list is
  not empty, fresh reads queue behind it instead of being answered first, so a backlog is never
  starved by new arrivals (with 19,500 overloaded connections this cut p99 from 370 to 325 ms).
  Socket queues (3–4 KB of kernel memory per waiting request) therefore empty at read speed,
  instead of pushing TCP towards its memory limit after a stall.
- One send per connection is in flight; later responses queue behind it. Output buffers (16 KB)
  come from a pool and go back once sent. Closing is a `SHUTDOWN` hard-linked to a direct-descriptor
  `CLOSE`, and a generation number in every completion tag drops completions for a reused slot.
  Idle connections are never closed by the server.

## Request path

- **HTTP:** one pass over the head finds the request line, `Authorization` and `Content-Length`
  (case-insensitive); a request split across receives is kept until complete. Heads over 16 KB,
  bodies over 1 MB, `Transfer-Encoding` and malformed request lines get 400 `bad request` and the
  connection is closed (out of scope in SPEC). Pipelined requests are answered in order.
- **JWT:** the HMAC key's inner and outer blocks are hashed once at startup. Each request hashes
  `header.payload` plus the outer block and compares in constant time. Only the canonical base64url
  spelling of a 32-byte signature is accepted. The header must be an object with `alg` `"HS256"`,
  and `exp` must be a number later than now (parsed with `strtod`). Then `sub` must be a string of
  a positive integer (no sign or leading zero) and `username` a string, else 401 `invalid token
  payload`. Escaped JSON strings in the header and payload are decoded.
- **JSON:** a strict reader (RFC 8259, UTF-8 validated, nesting up to 512) returns the requested
  top-level members; the last duplicate wins, escaped member names are decoded. Not an object →
  `body is required`; not JSON → `malformed JSON body`. `\uXXXX` escapes (with surrogate pairs) are
  decoded to UTF-8.
- `body` is trimmed with JavaScript's `trim()` whitespace set (ASCII plus U+00A0, U+1680,
  U+2000–U+200A, U+2028/9, U+202F, U+205F, U+3000, U+FEFF) and its length counted in code points.
- **Output:** strings are escaped 16 bytes at a time with SSE2 (`"`, `\` and control characters);
  integers are written two digits at a time. `Content-Length` is written as a 4-digit placeholder
  and fixed up when the body is done.

## Measurements (not the benchmark box)

Measured in a 2-vCPU cloud sandbox (Xeon @ 2.1 GHz, Linux 6.18) over loopback: server pinned to
one CPU, load generator (`tools/lg.c`, the k6 user's request mix) on the other. For a baseline,
rust-iouring-kensac was built in the same sandbox, against the same SQLite and with Rust 1.91
from Ubuntu's archive, then run through the same harness. Absolute numbers will differ on the
benchmark droplet.

| | this server | rust-iouring-kensac |
|---|---|---|
| 18,000 connections at ~18k req/s: server CPU per request | 15.6–16.1 µs | 18.0–19.2 µs |
| 18,000 connections at ~36k req/s: server CPU per request | 13.9–14.4 µs | 15.3–16.0 µs |
| 18,000 connections at ~18k req/s: p50 / p95 latency | 4 / 6 ms | 2 / 3 ms |
| 19,500 connections, closed loop (overload) | 86.7k req/s, p99 325 ms | 84.0k req/s, p99 334 ms |
| 200 busy connections, closed loop | 94–97k req/s | 88–94k req/s |
| two 2 s `SIGSTOP` stalls under load: worst request | 2,010 ms | 2,010 ms |

The latency difference is the 4 ms batching wait (see above); both are far inside the
500 ms / 1 s limits. About two thirds of the CPU per request is the kernel's (TCP and io_uring), which is the same for
both servers.

The normal-priority checkpoint tail, the page-cache fix and the INSERT without `RETURNING`, same
sandbox, 2,000 busy connections (closed loop, so writes are frequent and the server is
saturated), five runs of each build:

| | p99 | worst request | req/s |
|---|---|---|---|
| before (tail at nice 19) | 128–151 ms | 283–316 ms | 94–105k |
| after | 29–35 ms | 81–111 ms | 98–111k |

The old worst cases are write holds running into their 250 ms limit. The req/s ranges overlap: on
this sandbox the cache stays small (about 1,500 pages), so the cache walk the fix removes is short.

### On DigitalOcean droplets (not the benchmark box)

A Basic droplet of the benchmark box's size (s-1vcpu-2gb, Ubuntu 24.04, kernel 6.8.0-147) serving
directly, eight 1-vCPU load-generator droplets running the k6 user model (a C rewrite of
`bench/load.js`: same think times, mix and pass thresholds), and the leaderboard's search: 2-minute
probes doubling from 40,000, bisecting to ~5%, then 5-minute holds stepping down 5%. Both servers
ran on the same droplet, one after the other, each on a fresh copy of the seed. CPU is the server's
own cgroup, which leaves out interrupt handling and steal time, so the box was busier than shown.

**This code** (the second run):

| users | this server | rust-iouring-kensac |
|---|---|---|
| 40,000 | pass · 18% CPU · 43.5 µs/req | pass · 20% · 47.7 µs |
| 80,000 | pass · 34% · 40.3 µs · p99 56 ms | pass · 34% · 40.0 µs · p99 24 ms |
| 120,000 | pass · 47% · 37.4 µs · p99 86 ms | pass · 58% · 45.7 µs · p99 379 ms |
| 130,000 | pass · 57% · 41.6 µs · p99 795 ms | |
| 135,000 | pass · 56% · 39.5 µs · p99 863 ms | |
| 140,000 | fail · 59% · p99 1,014 ms | pass · 67% · 45.0 µs · p99 618 ms |
| 145,000 / 150,000 | | fail / fail |
| 160,000 | fail · 69% · p99 5,715 ms | fail · 77% · p99 3,603 ms |
| **5-minute hold** | **135,000: pass** · 14,279 req/s · 54% · 37.5 µs · p99 372 ms | 140,000 fail (p99 1,660 ms), 133,000 fail (p99 2,695 ms), **126,000: pass** · 13,297 req/s · 64% · 48.3 µs · p99 776 ms |

Same droplet, same search: this server held 135,000 users for 5 minutes, rust-iouring-kensac
(310,000 on the leaderboard) 126,000. A shared vCPU adds noise, and the search's 5% steps leave
each limit somewhere between its last pass and its first fail, so this is one sample.

The first run, on another droplet and before the checkpoint and page-cache changes above, held
125,000 users (p99 643 ms), and kensac passed 100,000 and failed 120,000 in 2-minute probes.

Correctness checks beyond `test/test.sh` (42/42):

- `/feed` and 3,000 random `/posts/:id` compared byte-for-byte with the reference SQL query (via
  Python's `sqlite3`) on a database edited to force the sort path, per-post like counts, a like
  range spanning several leaves, control characters, and the SQL fallback for a missing author.
- The JSON reader agrees with Python's strict `json` module on 60,014 fuzzed documents (verdict
  and decoded `body`); all 11,814 single-character mutations of a valid JWT are rejected.
- Every acknowledged write is in the database, and nothing else: with the checkpoint threshold
  lowered to 64 frames (500 write holds in 10 s, pipelined POSTs), after `kill -9` mid-load
  (three trials), and on a full disk (writes then get 500, never 201; reads and `/health` go on).
- 30,000 connections of mutated, randomly split and pipelined requests (corrupt bytes, 20 KB
  headers, bad lengths, resets): no crash, no descriptor or memory growth.
- Pipelining, byte-by-byte requests, 100 KB bodies, escaped and Unicode bodies, 19,500
  concurrent connections (a second ring is added), 7,000 connections over 4 rings.

`src/shim.c` has two test-only switches, off in the build: `-DCKPT_FRAMES=n` (checkpoint sooner,
to exercise write holds) and `-DTD_STATS` (log the hold count).

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

SQLite options (`build.sh`): `THREADSAFE=2` (each connection used by one thread),
`DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`,
`MAX_EXPR_DEPTH=0`, `USE_ALLOCA`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`,
`OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`, `OMIT_COMPLETE`,
`OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

## License

MIT, under the repo's [license](../../LICENSE).
