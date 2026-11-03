# Goose: an io_uring server written in Goose, SQLite B-tree reads

One thread serves every connection through io_uring and every request through one SQLite
transaction per pass. The server is [Goose](https://github.com/aardappel/goose), a memory-safe
systems language with no heap, no GC and no lifetime annotations, which compiles to C: the
event loop, the connection table, the accept and ring policy, batching and group commit, write
coalescing, the checkpoint policy, HTTP, JSON, HS256 and the read plans are all `app.goose`. C
is only what Goose cannot do: talk to the kernel (`uring.c`) and be SQLite (`sqlite.c`).

| | |
|---|---|
| Language | Goose, compiler pinned at commit `369c7a2c` (runtime `3025e5810379`), built from source by `install.sh` (sha256-checked) |
| Framework | none: the event loop, HTTP/1.1, JSON and HS256 (SHA-256 and HMAC included) are written in Goose here |
| io_uring | liburing 2.15, pinned tarball (sha256), static |
| SQLite | 3.54.1 amalgamation (sha256), unmodified, compiled into `sqlite.c` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, profile-guided, static |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors); tested on Ubuntu 24.04's 6.8 |

## Since the scored run (260,000 at `204b6b0`)

The first version's C was a port of rust-iouring-kensac's transport and helpers. This one is a
new design, and the transport moved into Goose:

- **The event loop is Goose.** `uring.c` decides nothing: it makes rings, queues the operation
  Goose asks for, copies completions out as flat records, and owns the bytes the kernel reads
  and writes while an operation is in flight (receive buffers, and numbered "boxes" for
  responses in flight and for requests that arrive in pieces). The connection table, what each
  completion means, accepts, ring spills, holds, flushing and closing are `app.goose`.
- **Why it held only 260k.** The box's report (270k and 280k failed on p99 at 4.9 s and 2.2 s
  with 0 errors and CPU to spare) reproduced locally: on one dedicated vCPU at 260,000
  connections and twice the user model's rate, the scored build failed (p99 2.4 s, 76 % CPU,
  WAL at 143 MB) where rust-iouring-kensac held (p99 22 ms). It was the WAL: commits every pass
  wrote many small sets of frames, the checkpointer at nice 19 fell behind, holds and the
  loop's own last-resort checkpoint followed. This version holds there with p99 24 ms and 3 %
  less CPU per request than kensac (measurements below).
- **Writes are coalesced** (below): one commit per window instead of one per pass.
- **The checkpoint policy is new and in Goose**, and the loop never checkpoints itself.
- More receive buffers per ring (4,096), so a long pass rarely runs a ring dry.

## What is Goose and what is C

| `app.goose` (Goose) | `uring.c`, `sqlite.c` (C) |
|---|---|
| the loop: waiting, draining every ring's completions, flushing after COMMIT, re-arming receives | `io_uring_enter`, queueing operations, copying completions into a Goose array |
| the connection table: generations, flags, which box holds what, partial and held requests | numbered byte boxes (C memory, since the kernel reads them asynchronously) |
| accepting: how many in flight, which ring, when a ring is full, when to open another | rings, sparse direct-descriptor tables, provided buffers, listening sockets |
| write coalescing, the checkpoint policy, when writes wait | the checkpoint thread (it copies when asked) |
| HTTP/1.1 framing and parsing, routing, every status code and error message | |
| HS256: base64url, SHA-256, HMAC (precomputed pad states), the claims | |
| a validating JSON reader (any document, escapes, surrogate pairs, UTF-8) and writer (8-byte SWAR escaping) | |
| the body's JavaScript `trim()` and code-point count | |
| SQLite's record format: varints, serial types, column ranges | moving SQLite's B-tree cursors, copying a row's record bytes |
| the feed and post read plans and their fallback to SQL | BEGIN/COMMIT, the two INSERTs, `SELECT 1` |

Goose cannot hold a pointer into memory it does not own, so the boundary is copies, and no
Goose memory is ever shared with the kernel: C appends a message's bytes and a row's record
bytes to Goose builders, and Goose's finished responses are copied into a box for sending.

## A pass

1. Every other ring submits and collects without waiting; ring 0 goes last and waits for 256
   completions or 1 ms, so a busy pass shares one COMMIT across many requests.
2. Each ring's completions are copied into a Goose array and handled in order. A receive is
   served at once: Goose parses every complete request in the message (after any bytes kept
   from an earlier one), answers it into the connection's output box, and keeps the rest.
3. Writes that are due run (below). Every request that touches the database joins the pass's
   transaction, begun by the first one.
4. `COMMIT`, then every touched connection's output goes to the kernel: **no response from a
   pass is sent before its COMMIT returns** (group commit, rule 6). If the commit fails, every
   answer of the pass becomes a 500 and its connection closes, so a 201 never claims a lost
   write.
5. Receives that ended are re-armed, accepts topped up, and the checkpoint policy runs.

## Connections

- Accepted sockets are io_uring **direct descriptors** in a ring's file table, so
  `LimitNOFILE=65535` caps a ring, not the server. A ring takes accepts until fewer than
  (accepts in flight + 64) slots are free, then the next ring with room takes over (one is
  added when none has room, up to 16 rings: over a million connections). Accepts are
  single-shot, 512 in flight, spread over eight `SO_REUSEPORT` listeners (eight accept queues).
- One **multishot receive** per connection into the ring's provided buffers (4,096 x 2 KB):
  an idle connection holds no buffer, only its 24-byte record in Goose's table.
- One send in flight per connection; answers to pipelined requests queue behind it, in order.
  Closing is a `SHUTDOWN` hard-linked to a direct `CLOSE`; a generation number in every
  `user_data` drops completions for a reused slot. Idle connections are never closed.
- Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration with zero reserved words
  (Launchpad #2162843); a refused registration is retried once with `resv[0] = 1`.
- The binary's code (static, ~2 MB) is `mlock`ed at startup: with no swap, reclaim near the
  memory wall takes file-backed pages first, and the loop must not fault its own code back in.

## Writes: coalesced, then group-committed

A write (`POST`) does not run in the pass it arrives in. It waits, unread, with its connection
(and so does anything pipelined behind it) until a window that opened with the first waiting
write ends, 2 ms later; then all waiting writes run in one pass and share its COMMIT. At the
load test's rate that is one commit with writes every couple of milliseconds instead of one per
pass, and the pages several writes touch (the tail of `posts`, of the likes table, of the
indexes on hot posts) go to the log once instead of once each. Locally (under a 35 % CPU cap,
260,000 users), turning the window off cost 19 % more CPU per request and took p99 from 270 ms
to 1.65 s. Each response still
waits for the commit of its own write (rule 6), and a write never waits longer than the window
plus one pass, or a checkpoint hold (below).

## SQLite

- **Reads walk SQLite's own B-tree cursors** inside the pass's transaction (only the bytecode
  engine is skipped; pages come through SQLite's pager, WAL and page cache), as the organizer
  allowed on 2026-10-31. `sqlite.c` moves a cursor and copies the row's record bytes into
  Goose; Goose decodes them and decides what to read next.
  - **Feed:** walk `posts_created_at_id_idx` from its first entry (it is `created_at DESC, id
    DESC`, so this is exactly the reference `ORDER BY`), then the 20 posts by rowid in
    ascending order (consecutive ids are the next cell of the same leaf), the 20 authors in
    `users`, and the like counts: one range of `likes_post_id_idx` counted in place on its leaf
    pages when the ids are within 512 of each other, a seek per post otherwise.
  - **Post:** a rowid seek on `posts`, one on `users`, and a like count.
  - **Nothing about the data is assumed**: not dense ids, not timestamps in id order. A post
    whose user row is missing (which the JOIN would drop) sends the feed to the reference SQL
    query. The column layout of every table and index decoded is checked at startup.
- **Writes are SQL**: `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`
  (a new like is told from a repeat by `sqlite3_changes`); the like's 404 check is a seek.
- WAL, `synchronous=NORMAL`, `unix-excl` VFS (the WAL index in heap memory, no per-transaction
  file locks), 32 MB page cache, `mmap_size=0`, `wal_autocheckpoint=0`.

## Checkpoints

A thread on its own connection, at nice 19 (so it runs in the loop's idle time), runs a PASSIVE
checkpoint whenever `app.goose` asks; the policy is Goose's:

- Past 8,192 log frames, ask for one. When it finishes, hold writes (reads go on) and ask for
  another, which copies the frames written meanwhile; when that one has copied the whole log,
  the next write starts the log over. A hold lasts at most 150 ms.
- If the log passes 24,576 frames while a checkpoint runs (the loop leaving the thread no idle
  time), writes are held too, and the loop gives the CPU away for 5 ms of every 50 until the
  log is back under, so an overload cannot grow the log without bound.
- The loop never runs a checkpoint itself.

## HTTP, JSON, JWT

- Only the headers the API uses are read. Requests split across receives are completed from
  the connection's box; pipelined requests are answered in order. Heads over 16 KB, bodies over
  1 MB, chunked bodies and malformed request lines get `400 bad request` and the connection
  closes (out of scope in SPEC). `Connection: close` and HTTP/1.0 close after the response.
- HS256 only; every token's signature is checked on every request (constant-time compare);
  only the key's padded blocks are hashed once at startup (the key schedule, not a result).
  Only the canonical base64url spelling of a signature is accepted. `exp` is required and must
  be in the future, then `sub` must be a positive integer string and `username` a string.
- The JSON reader validates whole documents (any nesting, with an explicit stack so no input
  can recurse), decodes escaped member names, and the last duplicate key wins, as in
  `JSON.parse`. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code
  points (the unit of the schema's `CHECK (length(body) …)`).

## Build

`install.sh` (root, once): `build-essential cmake ninja-build curl unzip python3`, then the
Goose compiler from its pinned source. It is one large C++20 translation unit; built with one
job at `-O2` and a tighter gcc collector it peaks at 1.34 GB (measured under a 1.6 GB cgroup
cap). It fit on the box for the scored run.

`build.sh` (user): fetches and checks SQLite and liburing, compiles `app.goose` to C
(`goose -O2 --standalone`), and builds `bin/server` static at `-O3` (`-march=x86-64-v3
-mtune=cascadelake`, 256-bit vectors, Ubuntu's default hardening off). **Profile-guided**: an
instrumented build serves `train/train.py`, which makes a synthetic database of its own (5,000
users, 40,000 posts, 160,000 likes, never the seed), signs its own tokens and drives the load
test's request mix plus the test suite's error paths. `PGO=0 bash build.sh` skips it. The
SIGTERM-to-`exit()` hook that writes the profile (`train/pgo_exit.c`) is linked into the
instrumented build only.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process; schema untouched; reads through SQLite's B-tree
cursors and pager, writes in SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: every request reads its rows from SQLite while it is served.
Kept between requests: prepared statements, cursor objects, `KeyInfo`, the four root page
numbers (schema metadata read at open) and the HMAC key schedule. Scratch buffers and boxes are
reused as storage only.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for the commit of its pass.
7. Nothing detects the load test; write coalescing applies to every write, whatever its route.
8–9. Direct on `HOST:PORT`.
10. Built from source on the box; Goose, SQLite and liburing pinned by commit/version and sha256.
11. Nothing on the box is tuned; the process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local measurements (not a score)

Not the organizer's box: an Ubuntu 24.04 arm64 VM (Lima on an Apple M4, kernel 6.8.0-142), the
server alone on one vCPU under systemd (`AllowedCPUs=0`, `MemoryMax=1800M`, `LimitNOFILE=65535`,
so 65,535-slot rings as on the box), a native replay of `bench/load.js`'s user model (one
keep-alive connection per user, the same request mix) on the other three vCPUs over loopback,
a fresh seed copy per run, 60 s ramp and 60 s hold. An M4 core is about three times the box's,
so think times are divided by 2 (or more) to put the server's vCPU at the box's utilization.
CPU is the server cgroup's over the hold. rust-iouring-kensac was built from its folder here.

260,000 users at twice the user model's rate (55,000 requests/s), where the box failed the
scored build:

| Server | p99 | µs/request | CPU | log at the end |
|---|---:|---:|---:|---:|
| **this version** | **24.3 ms** | **10.27** | 57 % | 56 MB |
| the scored build (`204b6b0`) | 2,359 ms | 14.50 | 76 % | 143 MB |
| rust-iouring-kensac (#264, 310,000 on the box) | 21.8 ms | 10.57 | 58 % | 24 MB |

300,000 users: at 2.6x (82,000 requests/s) this version held (p99 244 ms, 9.63 µs/request,
79 % CPU) where kensac did not (p99 1,310 ms); at 2.3x both failed (p99 1,376 / 1,130 ms).
That a lighter load failed where a heavier one passed means at least one intermittent stall is
left; it is not explained yet. At 3x (overload: every server past its CPU) this version still
served 66,500 requests/s at 14.6 µs each, against 42,700 at 23.3 for kensac and 37,500 at 26.0
for the scored build; overload costs grow, and growing less is what keeps a server inside the
p99 limit at the edge.

Behaviour, request for request against rust-iouring-kensac on fresh seed copies: 86
hand-written raw-request cases (pipelining, requests split byte by byte, every token failure,
escapes, surrogate pairs, Unicode trimming, multi-byte 500-character limits, duplicate keys,
nested members, invalid UTF-8, chunked bodies) give byte-identical status and body except one,
a body nested 300 arrays deep, which this entry accepts (as `JSON.parse` does) and kensac
rejects. A ring-spill run with 2,000-slot tables (`GOOSE_SLOTS`, test only) served 10,000 users
across six rings with no errors.

Test-only knobs, never set on the box: `GOOSE_SLOTS`, `GOOSE_WRITE_US`, `GOOSE_CK_FRAMES` and
`GOOSE_STATS` (a stats line every 5 s).

## Credit

Ideas from earlier entries, all MIT, with the code written for this entry: direct descriptors
in several rings, single-shot accepts kept in flight over `SO_REUSEPORT` listeners, multishot
receives into provided buffers, the buffer-ring retry for Ubuntu's kernels, and holding writes
while a checkpoint copies the log's tail come from rust-iouring-kensac (#264); counting likes
on the index's leaf pages, locking the code in memory, hardening-off flags and PGO on a
synthetic database from c-iouring-pavlyhalim (#245) and mojo-iouring-oh-meh (#257); reading
through SQLite's B-tree cursors from lean-derive-hargup (#153), as the organizer ruled.

## Files

| | |
|---|---|
| `app.goose` | the server: the loop and connection table, accepts and rings, holds, coalescing, checkpoint policy, HTTP, routing, JWT/SHA-256/HMAC, JSON, record decoding, read plans |
| `uring.c` | the kernel: rings, queued operations, completions, receive buffers, byte boxes, listeners |
| `sqlite.c` | SQLite: the amalgamation, cursor moves and row copies, the pass's transaction, writes, the checkpoint thread |
| `train/train.py`, `train/pgo_exit.c` | PGO training (build time only) |
| `install.sh`, `build.sh`, `start.sh` | |
