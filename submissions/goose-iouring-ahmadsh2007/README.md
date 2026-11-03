# Goose: io_uring + SQLite B-tree reads, every request in Goose

One thread serves every connection through io_uring and every request through one SQLite
transaction per pass. The request path is [Goose](https://github.com/aardappel/goose), a
memory-safe systems language with no heap, no GC and no lifetime annotations, which compiles
to C. C is only the plumbing Goose cannot do itself: io_uring's rings and buffers, and SQLite.

| | |
|---|---|
| Language | Goose, compiler pinned at commit `369c7a2c` (runtime `3025e5810379`), built from source by `install.sh` (sha256-checked) |
| Framework | none: HTTP/1.1, JSON, HS256 (SHA-256 and HMAC included) are written in Goose here |
| io_uring | liburing 2.15, pinned tarball (sha256), static |
| SQLite | 3.54.1 amalgamation (sha256), unmodified, compiled into `db.c` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, profile-guided, static |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors); tested on Ubuntu 24.04's 6.8 |

## What is Goose and what is C

| `app.goose` (Goose, ~1,200 lines of code) | `net.c`, `db.c` (C) |
|---|---|
| the pass: when to wait, when a write must wait out a checkpoint, when to commit | io_uring setup, SQEs and CQEs, provided buffers, the connection table |
| HTTP/1.1 framing and parsing (request line, `Authorization`, `Content-Length`, `Connection`, pipelining, partial requests) | copying a connection's received bytes into a Goose buffer, and its response out |
| routing, every status code and error message | |
| HS256: base64url, SHA-256, HMAC (with precomputed pad states), the claims | |
| a validating JSON reader (any document, escapes, surrogate pairs, UTF-8) and the JSON writer (8-byte SWAR escaping) | |
| the body's JavaScript `trim()` and code-point count | |
| SQLite's record format: varints, serial types, column ranges | positioning SQLite's B-tree cursors and copying a row's record bytes into a Goose buffer |
| the feed and post read plans: which cursor, which key, in which order, the fallback to SQL | BEGIN/COMMIT, the two INSERTs, `SELECT 1`, the checkpoint thread |

Goose cannot hold a pointer into memory it does not own, so the boundary is copies: C appends
a connection's new bytes and a row's record bytes to Goose builders (a few hundred bytes each),
and appends Goose's finished response to the connection's output. No request logic is in C.

## Connections

The transport follows the design of the leading entry (rust-iouring-kensac, #264):

- One thread drives every ring (`SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL`).
- Accepted sockets are io_uring **direct descriptors** in each ring's file table, so
  `LimitNOFILE=65535` caps a ring, not the server. When a ring is within a margin of full,
  accepting moves to the next ring (made when first needed, up to 16: over a million
  connections). Accepts are single-shot, 512 in flight, so a fast ramp never waits out SYN
  retransmits, and fewer than the margin, so none can land on a full table.
- Eight `SO_REUSEPORT` listeners: eight accept queues of `somaxconn` each.
- One **multishot receive** per connection into a provided-buffer ring (1,024 x 2 KB per ring):
  an idle connection holds no receive buffer and no output buffer, only a 40-byte record.
- One send in flight per connection; pipelined responses queue behind it. Closing is a
  `SHUTDOWN` hard-linked to a direct `CLOSE`; a generation number in every `user_data` drops
  completions for a reused slot.
- Each pass gives every other ring one non-blocking enter, then waits on ring 0 for 256
  completions or 1 ms, so a busy pass shares one COMMIT across many requests.
- Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration with zero reserved words
  (Launchpad #2162843); a refused registration is retried once with `resv[0] = 1`.
- The binary's own code (static, ~2 MB) is `mlock`ed at startup: with no swap, reclaim near the
  memory wall takes file-backed pages first, and the loop must never fault its own code back in.
- Idle keep-alive connections are never closed by the server.

## A pass

1. `net_wait`: submit what the last pass queued, wait, reap every completion. Accepts, sends and
   closes are bookkeeping in C; receives are queued in order (data and end-of-stream alike).
2. `net_next`, in a Goose loop: the next connection with new bytes, appended (after any bytes
   kept from an incomplete request) to a Goose buffer. Goose parses and answers every complete
   request in it, keeps the rest, or holds it (below).
3. Every request that touches the database joins the pass's transaction (begun by the first).
4. `COMMIT`, then `net_flush` hands every response to the kernel. **No response from a pass is
   sent before its COMMIT returns** (group commit, rule 6). If the commit fails, every response
   from the pass becomes a 500 and its connection closes, so a 201 never claims a lost write.

## SQLite

- **Reads walk SQLite's own B-tree cursors** inside the pass's transaction (only the bytecode
  engine is skipped; pages come through SQLite's pager, WAL and page cache), the approach the
  organizer allowed for the Lean entry (PR #153). `db.c` positions a cursor and copies the
  row's record bytes into Goose; Goose decodes them.
  - **Feed:** walk `posts_created_at_id_idx` from its first entry (it is `created_at DESC, id
    DESC`, so this is exactly the reference `ORDER BY`), then the 20 posts in ascending id
    order (consecutive ids are the next cell of the same leaf, SQLite's own `TableMoveto` fast
    path), the 20 authors in `users`, and the like counts in one range walk of
    `likes_post_id_idx` when the ids are within 512 of each other (a seek per post otherwise).
  - **Post:** a rowid seek on `posts`, one on `users`, and a like count.
  - **Nothing about the data is assumed**: not dense ids, not timestamps in id order. A post
    whose user row is missing (which the JOIN would drop) sends the feed to the reference SQL
    query. The column order of every table and index decoded is checked at startup, and the
    server refuses to start on a different schema.
- **Writes are SQL**: `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`
  (a new like is told from a repeat by `sqlite3_changes`); the like's 404 check is a B-tree seek.
- WAL, `synchronous=NORMAL`, opened through the `unix-excl` VFS (an exclusive lock held for the
  process's life; the WAL index is in heap memory, with no per-transaction file locks).
  32 MB page cache, `mmap_size=0` (a cached page keeps its parsed header; an mmap page pays a
  WAL-index lookup on every fetch).
- **Checkpoints never run on the loop.** A second thread, at nice 19 and on its own connection,
  runs PASSIVE checkpoints from 2,048 WAL frames. When one finishes, writes are held (reads go
  on) while it copies the frames written meanwhile, so the next write restarts the log; a
  held write stays unread in its connection until the hold ends (at most 250 ms). If the log
  passes 16,384 frames while a checkpoint is still running (the loop leaving the thread no CPU),
  writes are held then too; past 32,768 the loop checkpoints itself as a last resort.

## HTTP, JSON, JWT

- Only the headers the API uses are read. Requests split across receives are completed in a
  per-connection buffer; pipelined requests are answered in order. Heads over 16 KB, bodies over
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
- Output strings are scanned 8 bytes at a time (SWAR) and copied in runs.

## Build

`install.sh` (root, once): `build-essential cmake ninja-build curl unzip python3`, then the
Goose compiler from its pinned source. The compiler is one large C++20 translation unit; built
with one job at `-O2` and a tighter gcc collector it peaks at 1.34 GB (measured under a 1.6 GB
cgroup cap, 1.5 minutes on an M4 core; a few minutes on the box).

`build.sh` (user): fetches and checks SQLite and liburing, compiles `app.goose` to C
(`goose -O2 --standalone`), and builds `bin/server` static with `-O3`
(`-march=x86-64-v3 -mtune=cascadelake`, 256-bit vectors, Ubuntu's default hardening off).
**Profile-guided**: an instrumented build serves `train/train.py`, which makes a synthetic
database of its own (5,000 users, 40,000 posts, 160,000 likes, never the seed), signs its own
tokens and drives the load test's request mix plus the test suite's error paths, about 30 s in
all. `PGO=0 bash build.sh` skips it. The SIGTERM-to-`exit()` hook that writes the profile
(`train/pgo_exit.c`) is linked into the instrumented build only.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process; schema untouched; reads through SQLite's B-tree
cursors and pager, writes in SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: every request reads its rows from SQLite while it is served.
Kept between requests: prepared statements, cursor objects, `KeyInfo`, the four root page
numbers (schema metadata read at open) and the HMAC key schedule. Scratch buffers are reused as
storage and cleared per request.
6. WAL + `synchronous=NORMAL`, group commit, each response waits for its pass's commit.
7. Nothing detects the load test.
8–9. Direct on `HOST:PORT`.
10. Built from source on the box; Goose, SQLite and liburing pinned by commit/version and sha256.
11. Nothing on the box is tuned; the process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local measurements (not a score)

Not the organizer's box: an Ubuntu 24.04 arm64 VM (Lima on an Apple M4, kernel 6.8.0-142), the
server confined to one vCPU under systemd (`AllowedCPUs=0`, `MemoryMax=1800M`,
`LimitNOFILE=65535`, so 65,535-slot rings as on the box), a native replay of `bench/load.js`'s
user model (one keep-alive connection per user, the same think times and request mix) on the
other three vCPUs over loopback, each run on a fresh seed copy. CPU is the server cgroup's over
the hold; µs/request is that CPU time over requests. Loopback charges part of the client's TCP
work to the server and an M4 core is much faster than the box's, so these numbers are only
comparable with each other. The two other servers were built from their own folders in this
repo, in the same VM (the Rust entry with Ubuntu's Rust 1.91, its pinned 1.94 aside).

200,000 users (think times ÷ 1.5, so 31,750 requests/s), 60 s ramp, 40 s hold, runs alternated:

| Server | µs/request | p95 | p99 | errors | peak memory |
|---|---:|---:|---:|---:|---:|
| **this entry** | **8.73, 8.66** | 4.16, 4.06 ms | 6.98, 6.34 ms | 0 | 434, 423 MB |
| rust-iouring-kensac (#264, 310,000 on the box) | 8.98, 8.84 | 4.54, 4.16 ms | 50.2, 6.91 ms | 0 | 515, 448 MB |
| c-iouring-pavlyhalim (#245, 295,000 on the box) | 9.36 | 3.87 ms | 6.02 ms | 0 | 852 MB |

300,000 users (31,765 requests/s, five rings each), 90 s ramp, 60 s hold: this entry 9.77
µs/request, p99 11.1 ms, 0 errors, 647 MB; rust-iouring-kensac 9.72 µs/request, p99 9.6 ms,
0 errors, 741 MB. Per-request cost rises with the connection count for both (the kernel's
socket structures stop fitting in cache).

The pass wait was measured at 250 µs / 500 µs / 1 ms / 2 ms / 4 ms (9.16 / 8.62 / 8.44 / 8.85 /
9.20 µs/request at 200,000 users), so it stays at 1 ms. LTO across Goose and C measured no
difference and is not used.

**Behaviour** was compared request for request with rust-iouring-kensac on fresh seed copies:
86 hand-written cases (pipelining, requests split byte by byte, every token failure, escapes,
surrogate pairs, Unicode trimming, multi-byte 500-character limits, duplicate keys, nested
members, invalid UTF-8, chunked bodies) gave byte-identical status and body except one: a body
nested 300 arrays deep, which this entry accepts (as `JSON.parse` does; it allows 512 levels)
and the Rust entry rejects. A ring-spill run with 2,000-slot tables (test knob below) served
10,000 users across six rings with no errors.

Test-only knobs, never set on the box: `GOOSE_SLOTS_OVERRIDE` (smaller descriptor tables, to
exercise ring spills) and `GOOSE_WAIT_US` (the pass wait, for the comparison above).

## Credit

Earlier entries' published code, all MIT. `db.c`'s B-tree helpers (cursor setup, `seek_ge`,
`bt_count_likes`'s leaf walk, the payload copy), its checkpoint thread and write-hold state
machine, and the feed plan that `app.goose` implements are adapted from rust-iouring-kensac
(#264)'s `csrc/shim.c` and `src/db.rs`; `net.c` is a C port of the transport in its
`src/main.rs`. The hardening-off compiler flags, locking the code in memory, and PGO on a
synthetic database follow c-iouring-pavlyhalim (#245) and mojo-iouring-oh-meh (#257). The
Goose program (`app.goose`: HTTP, JSON, HS256 with SHA-256 and HMAC, record decoding, routing,
responses) is written for this entry.

## Files

| | |
|---|---|
| `app.goose` | the server: the pass, HTTP, routing, JWT/SHA-256/HMAC, JSON, record decoding, read plans |
| `net.c` | io_uring transport: rings, accept/receive/send/close, buffers, connection table |
| `db.c` | SQLite: the amalgamation, B-tree cursor helpers, transactions, writes, checkpoint thread |
| `train/train.py`, `train/pgo_exit.c` | PGO training (build time only) |
| `install.sh`, `build.sh`, `start.sh` | |
