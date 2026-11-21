# Zig + io_uring + SQLite

| | |
|---|---|
| Language | Zig 0.15.2 (official Linux tarball, SHA-256 pinned in `install.sh`), `ReleaseFast`, native CPU |
| Framework | none: one `io_uring` event loop on `std.os.linux.IoUring`, hand-written HTTP/1.1, JSON and JWT code |
| SQLite | 3.54.1, the unmodified official amalgamation (SHA-256 pinned in `build.sh`), compiled by GCC 13.3 (Ubuntu 24.04's `build-essential`) with `-O3`, profile-guided, and linked into the binary |
| C | `src/btree.c` only (97 lines): it includes the amalgamation and exports thin wrappers for SQLite's B-tree functions, which are `static` inside it. No server logic |
| Other dependencies | none (SHA-256/HMAC, base64url and JSON are in `src/main.zig`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

This is a port of our C entry, [`c-iouring-unorig-username`](../c-iouring-unorig-username), to Zig: the same design,
written by hand in Zig, with only the SQLite glue in C. Needs Linux 6.1 or newer with io_uring enabled (Ubuntu
24.04's 6.8 kernel has it on by default).

## Running it

```bash
sudo bash install.sh   # Zig 0.15.2 into /opt, plus build-essential, curl, unzip, xz-utils
bash build.sh          # fetches and verifies SQLite, builds bin/server (46 s on one arm64 core)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
zig test -lc -Ideps/sqlite-amalgamation-3540100 src/main.zig   # optional: escaping, HMAC and record-decoding checks
```

## What is where

| File | Lines | What it does |
|---|---:|---|
| `src/main.zig` | 2,115 | everything the server does: the io_uring loop and its ring set-up, connections, HTTP, JSON, JWT, routing, response building, pass scheduling, group commit, the B-tree read plans and record decoding, the checkpoint worker, the WAL write gathering |
| `src/train.zig` | 49 | the profile trainer, built only during `build.sh` (below) |
| `src/btree.c` | 97 | `#include "sqlite3.c"` plus wrappers: open the four cursors in the pass's transaction, move them (first, next, seek a rowid, seek the likes index), hand back the record bytes under a cursor, a read-only view of the likes index page under its cursor, and the variadic `sqlite3_config` call for the page cache |

Everything above the B-tree calls is Zig: records are decoded from SQLite's documented record format in
`src/main.zig`, and the likes walk reads the index page's cells itself through the page view. `io_uring` comes from
`std.os.linux.IoUring`; what it lacks is written in `src/main.zig` on raw syscalls: the timed wait
(`IORING_ENTER_EXT_ARG`), registered ring descriptors, and the provided-buffer ring registration with the retry below.

## Design

The design is the C entry's (its README has the measurements behind each choice).

- **One event-loop thread.** The box has one vCPU, so more loops would only add context switches. A second thread
  only runs checkpoints. Every ring is `DEFER_TASKRUN` and `SINGLE_ISSUER`, so a ring's completion work runs when the
  loop enters that ring and never interrupts a pass.
- **Wake-ups are batched when busy.** If the last pass handled 4 or more completions, the wait asks for ring 0's share
  of 128 completions (capped at a third of open connections) or 2 ms, whichever comes first; otherwise it wakes on the
  first one. Each wake-up then carries more requests, so the syscalls, the batch's `COMMIT` and the send submissions are
  shared. It only delays when responses are sent, never what they contain.
- **Accepted sockets are registered (direct) descriptors.** Accepts use `IORING_FILE_INDEX_ALLOC`, so a client
  occupies a slot in the ring's registered file table, not the process descriptor table. When a table is full the
  loop adds another ring (same thread, up to 32).
- **Eight `SO_REUSEPORT` listeners, single-shot accepts, 64 in flight on each**, issued only while a table has room,
  so an accept never finds its table full.
- **Multishot receives into provided buffer rings** (2,048 x 1 KB per ring). If a ring runs dry (`ENOBUFS`) the
  connection goes on a read queue.
- **A transaction per completion batch (group commit).** The first request in a batch that touches the database opens
  the transaction; every read and write in the batch runs inside it, and the batch ends with one `COMMIT`. Responses
  are written into an arena and only handed to `send` after that `COMMIT` returns, so a `201` always means the row is
  committed. If a commit fails, nothing from a batch with writes is sent: those connections are closed instead.
- **Sends go through io_uring**, one in flight per connection so responses stay in order. Each send owns a copy of
  its bytes, taken from a LIFO pool of 8 KB buffers. Closing a connection cancels its receive (and send) by
  completion tag and closes the slot in one linked chain.
- **Reads use SQLite's B-tree cursors, not its SQL engine.** `/feed` walks `posts_created_at_id_idx` for the 20 newest,
  counts their likes in one walk of `likes_post_id_idx` (reading a leaf's cells directly), fetches the rows in
  ascending id order, then looks up each author. `/posts/:id` is the same plan for one row. Every request still reads
  its rows from SQLite while it is served (rule 5).
- **SQL is the fallback.** At startup the server checks the columns and sort order of everything it decodes; on any
  difference every read uses the reference SQL. At run time any record it doesn't expect (overflow pages, a post
  without a user, a column of another type, a missing post) sends that request to SQL. Writes always use prepared SQL.
- **A worker thread copies the log back, and the event loop decides when.** The worker (a `std.Thread` at nice 19) runs
  `PASSIVE` checkpoints on its own connection through the `unix-excl` VFS when the loop asks over an eventfd (after
  8,192 frames); the outcome comes back on a second eventfd as a ring completion, so the loop never waits for an
  fsync. If frames arrived during a pass, POSTs stay unread for one more pass, for at most 250 ms (reads go on); past
  32,768 frames they wait for it. Every 8 MB of new log, the loop queues `SYNC_FILE_RANGE_WRITE` on it, so dirty WAL
  pages never pile up to the kernel's dirty threshold. Durability: WAL with `synchronous=NORMAL`.
- **One `pwrite` per WAL commit.** SQLite writes each log frame as two `pwrite`s (header, then page). Through the unix
  VFS's documented `xSetSystemCall` hook, the loop thread gathers a commit's frames and writes them in one `pwrite`
  once the commit frame is complete, before SQLite updates the log index; a read or `fstat` of the same file writes
  them out first. This and a send pool of 2,048 buffers (512 in the C entry) are our C entry's best local build
  (`4298dd7`, not yet scored on the rig); both sit behind one compile-time switch, `COMBO` at the top of
  `src/main.zig`. It is on; off gives the design the C entry was scored with.
- **Provided-buffer registration retries.** Ubuntu's 6.8.0-136 and later kernels reject a buffer ring whose reserved
  words are zero (Launchpad #2162843); the registration retries once with `resv[0] = 1`.
- **After a stall, reads and answers are bounded**: a pass does at most 4,096 queued reads and answers 1,024 requests,
  newly arrived ones first; the rest wait on their connection, on a first-in, first-out list.
- **Hot memory sits on huge pages** (`MADV_HUGEPAGE`): the 32 MB page cache (one region through
  `SQLITE_CONFIG_PAGECACHE`), the receive buffers, the connection tables and the response arena.
- **Profile-guided SQLite.** Zig has no PGO, so the profile covers the GCC side: `build.sh` compiles `src/btree.c`
  instrumented and links it into `src/train.zig`, which creates a synthetic database (20k users, 200k posts, 600k
  likes; the schema's tables and indexes, none of the challenge seed) and serves 300,000 requests in the benchmark mix
  through the server's own request code, committing every 64. `src/btree.c` is then recompiled with that profile and
  linked into the server. The trainer and its database are deleted.
- **No caching** (rule 5). The only thing computed once is the HMAC key schedule derived from `JWT_SECRET`.
- **Strict input handling**: an RFC 8259 JSON validator (duplicate keys: last wins), `\u` escapes and surrogate pairs
  decoded, invalid UTF-8 rejected, JavaScript's `String.prototype.trim` whitespace set, the 500-character limit in
  code points. JWTs: HS256 only, constant-time signature compare, `exp` (required) and `nbf` checked.
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and `Expect: 100-continue`.
  Chunked request bodies aren't supported (out of scope).
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard limit.

## Differences from the C entry

- **The B-tree calls cross an object boundary.** The C entry is one translation unit built with `-flto` and PGO over
  the whole program, so SQLite's cursor code is inlined into its feed plan. Here each cursor move and record fetch is
  a call into `src/btree.c`, and the Zig code gets no profile.
- **Escaping** scans with Zig vectors (`@Vector`) instead of the C entry's 8-byte SWAR test; the output is the same.
- **io_uring** is `std.os.linux.IoUring` plus the raw-syscall pieces above, not liburing.
- **Record decoding** (varints, serial types) is written in Zig from SQLite's file-format documentation instead of
  SQLite's internal macros.

## Local measurements

All on an Apple-silicon Mac in Colima (an arm64 Linux 6.8 VM), so none of it predicts a score on the benchmark box:

- `test/run.sh`: 42 of 42, in a clean `ubuntu:24.04` arm64 container after `install.sh` as root and `build.sh` as a
  normal user. `zig test`: 3 of 3.
- **Same responses as the C entry** (`4298dd7`). Both servers started on copies of the seed; 616 exchanges went to
  each in the same order: every route, 380 post ids, 25 broken or unusual tokens on both write routes, 34 post bodies
  (escapes, surrogates, NULs, 500 and 501 characters, duplicate keys, invalid UTF-8), likes with read-backs,
  pipelining, HTTP/1.0, `Connection: close`, `Expect: 100-continue`, split requests, chunked and malformed requests.
  All 616 raw responses, headers included, were byte-identical once the timestamps of rows written during the run and
  `uptime_s` were masked. As a check of the check, one extra like sent to one side only made 4 of them differ.
- **The B-tree path is taken.** Under `gdb`, 100 `/feed` and 100 `/posts/:id` requests ran 100 feed walks and no
  fallback statement.
- **Write path.** 300 sequential `POST /posts`: 303 `pwrite64` calls, the same as the C entry's `4298dd7` (about one
  per commit). `kill -9` after 600 acknowledged POSTs in bursts of 30: all 600 present after reopening,
  `integrity_check` ok (the same for the C entry).
- **Build**: 46 s on one arm64 core with 2 GB. On x86-64, `install.sh` and `build.sh` (profile run included) passed in
  an emulated `ubuntu:24.04` amd64 container; the server there opened the database and stopped at `io_uring` set-up,
  which the emulator does not provide.
- **Smoke load**: 20,000 users (the k6 user model, our own load generator), 60 s ramp and 30 s hold, one CPU: 0 errors,
  p99 4 ms, 3% CPU, the same as the C entry in the run after it. That load is far too light to tell the two apart.

Not measured: the server serving requests on x86-64, CPU per request against the C entry, and any load near the
leaderboard's levels. The C entry's measurements do not carry over to this port.

## Credits

The design and the code structure are our C entry's, ported by hand. That entry credits rust-iouring-kensac (#248,
#264) for B-tree cursor reads, the `unix-excl` VFS, the low-priority checkpoint thread with write holds, `SO_REUSEPORT`
listeners, `DEFER_TASKRUN` on every ring and bounded reads after a burst; the `key_info`, `rootpage`, `layout_is` and
likes-seek set-up here follow kensac's `csrc/shim.c` closely, by way of the C entry. The `resv[0]` retry is also from
rust-iouring-kensac and c-iouring-pavlyhalim (#245); the WAL writeback kick follows #516. Keeping the SQLite glue in a
C file that includes the amalgamation, under a server in another language, follows asm-btree-ninhnv (#388,
`src/shim.c`) and zig-iouring-harshitkhandelwal208 (#460).

## License

MIT, under the repo's [license](../../LICENSE).
