# Rust, no framework: io_uring + SQLite B-tree reads, feed off the table's tail

One thread drives everything: an io_uring event loop, an HTTP/1.1 parser, a JSON reader/writer,
HS256 verification, and SQLite (the pinned, unmodified amalgamation, linked in statically). The
one exception is WAL checkpoints, which a second, low-priority thread runs.

| | |
|---|---|
| Language | Rust 1.94.0 (official standalone tarball, sha256-checked by `install.sh`) |
| Crates | `io-uring` 0.7.15, `libc` 0.2.183, `sha2` 0.10.9, `memchr` 2.8.0, `cc` 1.2.56 (build); pinned in `Cargo.lock` |
| SQLite | 3.54.1 amalgamation, sha256-checked, compiled by `build.rs` with `-O3 -march=native` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings) |

## What is new here

This entry builds on **rust-iouring-kensac #264**: its io_uring transport, HTTP/JWT/JSON code, the
SQLite shim, the direct-descriptor ring scheme, the group-commit/durability design and the
bounded-read recovery path are reused under the repo's MIT license and remain the base. The changes
below are mine.

### 1. `GET /feed` reads the posts table's tail

If `created_at` never decreases with rowid, `ORDER BY created_at DESC, id DESC` is exactly reverse
rowid order, so the 20 newest posts are the last 20 rows of the `posts` B-tree. The plan is used
only when the actual data proves the invariant:

- At startup one scan of `posts` verifies `created_at` is non-decreasing in rowid order. The scan
  uses a prepared statement on the same connection, then calls `posix_fadvise(DONTNEED)` on the
  database file so the table's clean page-cache pages are not charged to the server's cgroup.
- Every `INSERT … RETURNING` compares its `created_at` with the previous insert's. If the timestamp
  ever goes backwards (or is not the 24-byte wire format), the tail plan is switched off for good.
  A timestamp that equals the previous one is fine: `(created_at DESC, id DESC)` still orders the
  tie by id.
- With the plan on, the feed is one `sqlite3BtreeLast` plus 20 `sqlite3BtreePrevious` steps. The
  record already holds `user_id`, `body` and `created_at`; the 20 authors are still rowid seeks in
  `users`, and the like counts are still one range walk of `likes_post_id_idx`.
- If the startup check fails, or a post's author row is missing (where the reference `JOIN` drops
  it), the entry falls back to #264's index plan / reference SQL. Every request still reads its
  rows from SQLite's B-tree while it is served. Nothing is remembered between requests except one
  boolean and the previous 24-byte timestamp used to maintain the invariant — no rows, no query
  results, no usernames.

The same tail plan appears in mojo-iouring-oh-meh #224/#257 and swift-iouring-akbashev #380.

### 2. No checkpoint work on the event loop

#264's last-resort path ran `sqlite3_wal_checkpoint_v2(PASSIVE)` on the event loop once the WAL
reached 65,536 frames (256 MiB). Copying that on the only CPU takes seconds on the box's disk; at
the capacity limit it is the multi-second stall that turns into a huge p99 (the #264 run at 320k
reported p99 23 s at 65 % CPU). That path is gone:

- The nice-19 checkpoint thread still runs PASSIVE checkpoints, and writes are still held for at
  most 250 ms so the thread can copy the tail and let the next write restart the log.
- If the thread cannot keep up, the loop keeps serving: the WAL grows and reads find the newest
  frame through the wal-index. At the scored write rate the WAL grows by about 12 MB/s (each pass
  dirties roughly 26 pages), so a five-minute hold that never resets costs about 3.6 GB of disk,
  versus a multi-second p99 on the loop. Durability is unchanged: every 201 still waits for its
  COMMIT, with WAL + `synchronous=NORMAL`.

### 3. Receive buffers: 2,048 × 1 KiB

#264's receive ring is 1,024 × 2 KiB. This entry doubles the count and halves the buffer size,
keeping the same 2 MiB per ring: 2,048 × 1 KiB. A request fits in 1 KiB. On the 1-CPU
official-mix saturation this measured **47.2–49.3k req/s** versus #264's 41.5k (+14–19%), with
lower p95 and p99.

The 4,096 × 1 KiB variant, per-socket `TCP_CONGESTION=reno`, and mlocking the executable's code
were slower and are not in this entry.

Everything else — accept with direct descriptors, 8 `SO_REUSEPORT` listeners and early ring
switching, multishot receive into provided buffers, bounded single-shot reads after a burst, one
send per connection, one SQLite transaction per pass with group commit, writes through SQL, no
mmap, `unix-excl` — is #264's design.

## Why reads go through SQLite's B-tree, not its SQL engine

`GET /feed` and `GET /posts/:id` read SQLite's own B-tree cursors (`csrc/shim.c` `#include`s the
unmodified amalgamation), inside the pass's SQLite transaction. Rust then decodes the record bytes
SQLite returns and writes the JSON directly. There is no bytecode engine, no `sqlite3_column_*`
call, and no copying through SQLite values. Writes go through SQL and the amalgamation is
unmodified.

`GET /posts/:id` is a rowid seek in `posts`, a rowid seek in `users`, and one range count of
`likes_post_id_idx`. Writes use `INSERT … RETURNING id, created_at` and
`INSERT … ON CONFLICT DO NOTHING`; `sqlite3_changes` separates a new like from a repeat. The like
handler's 404 check is a B-tree seek.

## Correctness checks

- `bash test/run.sh submissions/rust-iouring-fexaop`: **42/42**.
- Non-monotonic data: a copy of the seed with `created_at` set to 2027 on post 1000 makes the
  startup check fail; `/feed` then comes from the index plan and matches the reference SQL byte for
  byte (including the 2027 post at the top).
- After 200 `POST /posts` inserts the feed still matches the reference SQL byte for byte, and the
  insert guard keeps the tail plan on (all timestamps move forward).

## Local measurements (not the benchmark box)

One pinned core, 200 loopback keep-alive connections, the other cores running the load:

| | #264 baseline | this entry |
|---|---:|---:|
| feed B-tree + JSON, no HTTP (`/feed` in a loop), median of 3 | 4.20 µs | 3.84 µs |
| full `GET /feed` request, 15 s runs, app CPU, median of 3 | 10.50 µs | 10.22 µs |

The feed-only change is the tail plan; the full-request delta is that plus noise.

Server saturation on the dev host (1 CPU, 128 loopback VUs, 20 s, reduced CQ 16,384 and 8,192
direct descriptors):

| request mix | #264 baseline | this entry |
|---|---:|---:|
| official mix, ~8% writes (46% feed / 46% post read / 7% like / 1% create) | 41.5k req/s, p95 5.12 / p99 7.52 ms | **47.2–49.3k req/s (+14–19%), p95 4.10–4.32 / p99 6.05–6.65 ms** |
| feed only | 45.5k req/s | 39.2k req/s |
| deliberately write-heavy, 30% writes | 33.3k req/s | 32.0k req/s |

The repository's `bench/load.js` at 5,000 VUs / 30 s also passed both builds: #264 382.8 req/s,
p95 1.92 / p99 2.14 ms; this entry 381.7 req/s, p95 1.91 / p99 2.16 ms.

At 10,000 VUs / 60 s the load starts to separate:

| `bench/load.js`, 10k VUs | #264 baseline | this entry |
|---|---:|---:|
| req/s | 819.4 | 818.8 |
| p95 | 2.07 ms | **1.92 ms** |
| p99 | 6.23 ms | **2.50 ms** |
| max | 274.5 ms | **16.44 ms** |
| failed | 0 | 0 |

Same throughput, 60% lower p99 and a 16× lower max. That is the no-inline checkpoint and the
2,048-buffer ring showing up under the actual load.js workload. These VU levels are still far
below the 310k capacity hold; the official box is the proof.

The scored mix is the first row. The feed-only and write-heavy rows are not the scored shape: the
smaller receive buffer trades feed-only throughput for the mixed official load, and the removed
last-resort loop checkpoint lets the WAL grow at roughly ten times the official post rate. These
are dev-host runs, not a 300k-connection score. The capacity changes are the removed loop
checkpoint and the 2× receive-buffer count; the tail feed plan is the smaller, safe win. The
4,096-buffer, reno and mlock variants were measured slower and are not in this entry.

## Files

| | |
|---|---|
| `src/main.rs` | rings, accept/receive/send/close, the batch loop |
| `src/app.rs` | routes, handlers, responses |
| `src/db.rs` | record decoding, the tail/index feed plans, writes (FFI to the shim) |
| `src/http.rs`, `src/json.rs`, `src/jwt.rs` | request framing, JSON, HS256 |
| `csrc/shim.c` | the amalgamation plus B-tree cursor helpers, the startup invariant scan, the checkpoint thread |
| `build.rs` | compiles the shim and SQLite |

SQLite options (all in `build.rs`): `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`,
`DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`,
`USE_ALLOCA`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`,
`OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`, `OMIT_COMPLETE`,
`OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
