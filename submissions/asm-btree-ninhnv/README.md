# x86-64 assembly: io_uring + SQLite B-tree reads

A feed server whose application code is hand-written x86-64 assembly (NASM): the io_uring event
loop (raw `io_uring_setup`/`enter`/`register` syscalls on hand-mapped rings, no liburing), HTTP/1.1
framing, routing, a validating JSON reader, JSON output, SHA-256/HMAC-SHA256, base64url, the JWT
checks, and the decoding of SQLite's B-tree records. One thread drives everything except WAL
checkpoints, which two helper threads run.

The design follows [rust-iouring-kensac](../rust-iouring-kensac) (PR #264): one pass = one SQLite
transaction, reads through SQLite's B-tree cursors, direct descriptors, multishot receives, a
checkpoint thread with write holds, drain-first recovery after a stall. The event loop and the
application are re-implemented in assembly.

**Reused code:** `src/shim.c` is adapted from `submissions/rust-iouring-kensac/csrc/shim.c`
(PR #264): the `td_*` interface, the cursor helpers, the checkpoint thread's states and the
overflow (spill) buffer come from there. What this shim changes or adds is listed at the top of
the file. Ideas taken from other submissions and written anew here: the page-cache wrapper
(c-iouring-pavlyhalim #245, wat-wasmedge-delneg #272), syncing the database before a write hold
(swift-iouring-akbashev #376), locking the code in RAM (c-iouring-pavlyhalim #245,
mojo-iouring-oh-meh #257).

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
| `src/shim.c` | the amalgamation plus thin wrappers (adapted from rust-iouring-kensac's `csrc/shim.c`, see above): open, per-pass transaction, cursor moves, record bytes, a page view, the two INSERTs, `SELECT 1`, the SQL fallback, the checkpoint threads, the page-cache wrapper, the code lock |
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
  frames, a helper thread (nice 19, own connection) copies it back with PASSIVE checkpoints: the
  cycle's frames first, then, while the log is still under 8,192 frames, rounds of "sync the
  database file, copy what was written meanwhile" until at most 128 frames were written during a
  round or a round stops gaining. Then writes are briefly held: the next
  POST on each connection stays unread while a second helper thread copies the last frames, so the
  next write can restart the log from the top. Reads go on during a hold, and a hold lasts at
  most 250 ms. Past 65,536 frames the loop checkpoints by itself.
  - *Why the sync before the hold:* a copy that doesn't finish the log leaves its pages unsynced
    (SQLite syncs the database only when every frame is in), so the held copy had to flush the
    whole cycle while writes waited. On a slow disk the hold ran out first, the log was not
    restarted, and the next cycle began at once. With the disk modelled at 60 MB/s (every
    `fsync` taking 5 ms plus its unsynced bytes at that rate) and 240,000 users of the k6 model,
    hold-phase latency went from p99 89–91 ms and a 290–300 ms maximum (every hold ran its full
    250 ms) to p99 23–24 ms and a 90–116 ms maximum, with no hold running out (two runs each).
  - *Why the rounds are bounded:* they only pay off while the helper keeps up. On a busy vCPU a
    nice-19 thread gets little, each round takes longer than the one before, and no hold can
    start while rounds run, so the log grows instead of restarting. On a 1 vCPU / 2 GB droplet
    with 260,000 users the log reached 32,625 and 41,550 frames with unbounded rounds (two
    droplets), and 141,927 at 320,000; without any rounds it never passed 9,811 at any level.
    Bounded, it stayed under 14,100 in 2-minute probes up to 280,000 users, and write holds
    added up to 0.1–0.6 s a minute, against 1–2 s a minute without rounds.
  - *Why two threads:* the second one runs at normal priority because writes wait for it; at
    nice 19 it got ~1.5% of a busy CPU and holds ran out. After the rounds it has a handful of
    frames to copy and sync.
  - The log has a line a minute: holds (the longest, their sum, the extra rounds), holds that ran
    out, copies the loop made itself, the most frames the WAL held; and for the loop its longest
    pass with the CPU time that pass used, the passes over 100 ms and the longest commit. A long
    pass with little CPU time means the loop was blocked or kept off the CPU.
- **Page cache after a split.** When a write splits a B-tree page, SQLite renumbers the new pages
  through a temporary page number far past the end of the file, and its page cache keeps the
  largest number it has seen. Every later commit truncates the cache to the file size, and from
  that number the cache walks every page it holds (all 64 MB) to find nothing. SQLite's default
  page cache therefore sits behind its plugin interface (`SQLITE_CONFIG_PCACHE2`): the wrapper
  notes the page numbers that enter a cache and does not pass on a truncate that has nothing to
  drop. In a write-heavy run all 1,047 truncates in 5 seconds were skipped; afterwards
  `integrity_check` was `ok` and all 513,401 posts matched the reference query. SQLite's own
  state is never written from outside: cursors move only through `sqlite3Btree*` calls.
- **Code locked in RAM.** At startup the binary's and its libraries' code and read-only data
  (3.7 MB) are `mlock`ed. On the 2 GB box a few hundred thousand sockets take most of the memory;
  with no swap the kernel then reclaims file-backed pages first, and an unlocked loop stalls on
  major faults reading its own code back from disk. It fits the 8 MB `RLIMIT_MEMLOCK` of a systemd
  service (checked as an unprivileged user with that limit and four rings on kernel 6.8). If the
  kernel ever refuses a ring for that limit, the lock is dropped and the ring retried.
- **A crash leaves a line in the log** (signal, faulting address, instruction pointer) before the
  signal takes its default course.
- **Batch size.** Each pass waits on ring 0 for 256 completions or 1 ms, whichever comes first
  (never while reads or answers are queued). It was 4 ms: in the sandbox, at ~18k req/s over
  18,000 connections, that cut server CPU from 17.8 to 16.0 µs per request for 2–3 ms more median
  latency. Near a full vCPU it works the other way. A pass and its wait last about
  wait / (1 − load), so at 85% load the 4 ms wait became 25 ms of median latency. On a droplet
  with 260,000 users the 1 ms build ran at 76% CPU with a 5 ms median, the 4 ms build at 84% with
  25 ms (one probe each), and at 280,000 the three 1 ms builds passed the 2-minute probe where
  both 4 ms builds failed (below).

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

The latency difference is the 4 ms batching wait this server had when these were measured (it now
waits 1 ms, see above); both are far inside the 500 ms / 1 s limits. About two thirds of the CPU per request is the kernel's (TCP and io_uring), which is the same for
both servers.

The normal-priority checkpoint tail, the page-cache fix (then a reset of the cache's largest key
before each commit, since replaced by the wrapper above) and the INSERT without `RETURNING`, same
sandbox, 2,000 busy connections (closed loop, so writes are frequent and the server is
saturated), five runs of each build:

| | p99 | worst request | req/s |
|---|---|---|---|
| before (tail at nice 19) | 128–151 ms | 283–316 ms | 94–105k |
| after | 29–35 ms | 81–111 ms | 98–111k |

The old worst cases are write holds running into their 250 ms limit. The req/s ranges overlap: on
this sandbox the cache stays small (about 1,500 pages), so the cache walk the fix removes is short.

### On the benchmark box

PR #373 at `9f80f1f` (before the checkpoint rounds, the page-cache wrapper and the code lock):
**230,000 users** held for 5 minutes (22,362 req/s, p95 21.9 ms, p99 103.5 ms, 0 errors, 58% CPU,
24 µs per request, 581 MB peak). 2-minute probes: 200,000, 220,000 and 230,000 passed; 320,000
failed on latency (p99 1.14 s, 0 errors); 240,000 failed with refused connections.

PR #373 at `e1bee48` (as merged: unbounded checkpoint rounds, 4 ms wait): **260,000 users** held
for 5 minutes (24,997 req/s, p95 27.6 ms, p99 59.3 ms, 0 errors, 72% CPU, 26 µs per request,
650 MB peak). 270,000 and 280,000 failed their 2-minute probes on latency (1.4 s and 1.3 s, 0
errors); 320,000 reached a 3 s p99 at 96% CPU. The bounded rounds and the 1 ms wait are the
answer to that step from 59 ms to over a second.

### On DigitalOcean droplets (not the benchmark box)

A Basic droplet of the benchmark box's size (s-1vcpu-2gb, Ubuntu 24.04, kernel 6.8.0-147) serving
directly, eight 1-vCPU load-generator droplets running the k6 user model (a C rewrite of
`bench/load.js`: same think times, mix and pass thresholds), and the leaderboard's search: 2-minute
probes doubling from 40,000, bisecting to ~5%, then 5-minute holds stepping down 5%. Both servers
ran on the same droplet, one after the other, each on a fresh copy of the seed. CPU is the server's
own cgroup, which leaves out interrupt handling and steal time, so the box was busier than shown.

The code scored above (the second run):

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

**The bounded rounds and the 1 ms wait**, on a Premium Intel droplet (s-1vcpu-2gb-intel, same
kernel), six 4-vCPU load generators, every simulated user on its own source port. The merged
build was first run with 200,000 users and k6's think times; it used 66% CPU against about 55%
on the benchmark box, so think times and ramps were stretched 1.2 times. The merged build then
passed 260,000 at 72% CPU and failed 280,000, as on the box. 2-minute probes, p95 / p99 in ms:

| users | as merged | merged, no rounds | this version | this, no rounds | this, 16,384-frame cycle |
|---|---|---|---|---|---|
| 220,000 | 24 / 168 | 23 / 100 | 23 / **1,037** | 49 / **1,277** | 22 / 652 |
| 240,000 | **602 / 1,464** | 49 / 197 | 16 / 224 | 30 / 537 | 26 / 184 |
| 260,000 | 393 / 834 | 61 / 260 | 124 / 457 | **668 / 3,781** | **915 / 1,790** |
| 280,000 | **904 / 1,762** | **1,003 / 1,981** | 376 / 659 | 101 / 390 | 410 / 798 |

Bold fails. Median latency was 10–18 ms with the 4 ms wait and 3–5 ms with 1 ms. No build held
280,000 for 5 minutes here (this version: p99 2.5 s at 81% CPU, slow from its first minute where
the 2-minute probe at the same level was not), so this is a noisy box near its limit and each
cell is one sample. What the log lines add: in this version's probes no commit took over 10 ms
and no write hold over 129 ms; the passes over 100 ms sit in the minute where the ramp ends,
and in them the loop got about half the CPU. About a fifth of the vCPU was user code (this
server and SQLite); the kernel's TCP and io_uring work took the rest of what was used.

Correctness checks beyond `test/test.sh` (42/42):

- `/feed` and 3,000 random `/posts/:id` compared byte-for-byte with the reference SQL query (via
  Python's `sqlite3`) on a database edited to force the sort path, per-post like counts, a like
  range spanning several leaves, control characters, and the SQL fallback for a missing author.
- The JSON reader agrees with Python's strict `json` module on 60,014 fuzzed documents (verdict
  and decoded `body`); all 11,814 single-character mutations of a valid JWT are rejected.
- Every acknowledged write is in the database, and nothing else: with the checkpoint threshold
  lowered to 64 frames (500 write holds in 10 s, pipelined POSTs), after `kill -9` mid-load
  (three trials; three more on the slow-disk model with the checkpoint rounds, 31,841–42,121
  posts and as many likes each; three more with the bounded rounds, 7,584–19,733 each), and on a
  full disk (writes then get 500, never 201; reads and
  `/health` go on).
- After 2.3 million requests of mixed load with the page-cache wrapper: `integrity_check` ok, and
  all 522,289 posts (with their like counts) and the feed match the reference query.
- 240,500 keep-alive users of the k6 model over 13 rings: no errors, also with the server
  stopped for 2.5 s six times (55,000 requests waiting each time) and with every `fsync` taking
  0.3–2.5 s.
- 250,000 short connections ending in every way a client can (unread responses, resets, half
  closes, partial requests, garbage, 60 pipelined requests then a reset) beside 58,500 keep-alive
  users over 4 rings, with write holds every 64 frames (1,042 holds) on the slow-disk model: no
  crash, no failed request among the keep-alive users.
- 30,000 connections of mutated, randomly split and pipelined requests (corrupt bytes, 20 KB
  headers, bad lengths, resets): no crash, no descriptor or memory growth.
- Pipelining, byte-by-byte requests, 100 KB bodies, escaped and Unicode bodies, 19,500
  concurrent connections (a second ring is added), 7,000 connections over 4 rings.

`src/shim.c` has two test-only switches, off in the build: `-DCKPT_FRAMES=n` (checkpoint sooner,
to exercise write holds) and `-DBULK_NICE=n` (the copying thread's priority).

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
