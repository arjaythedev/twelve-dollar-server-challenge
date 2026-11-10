# swift-iouring-akbashev

| | |
|---|---|
| Language | Swift 6.4.2 (official Ubuntu 24.04 tarball, sha256-checked), Swift 6 language mode |
| Framework | none: its own io_uring layer (raw system calls, no liburing), HTTP/1.1 server, JSON and JWT |
| SQLite | 3.54.1 amalgamation (sha256-checked), unmodified: the server's one C target; its B-trees read directly from Swift |
| Dependencies | none besides SQLite: the standard library, Synchronization and Glibc; no Foundation, no Dispatch |
| Nginx or direct | **direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the box) |

## Design

- **io_uring without liburing or C.** `Sources/IORing` is a module of its own: `io_uring_setup`, `io_uring_enter` and
  `io_uring_register` through `syscall(2)` (declared with `@_extern(c)`, like the other Linux calls in
  `Sources/Syscalls`), the rings mapped with `mmap`, entries and completions written and read at the kernel's offsets,
  and the heads and tails moved through `Atomic` with the acquire and release orderings the ring protocol asks for.
  Its per-request calls are `@inlinable` and `@inline(__always)`, so the reactor's compile as its own. The type is
  shaped like swift-system's `IORing` (prepare requests, submit, consume completions) with the socket requests that
  one doesn't have yet: multishot accept into direct descriptors, multishot and single receives into provided buffers,
  send, shutdown and close. The ring is `SINGLE_ISSUER` and `DEFER_TASKRUN`: one thread submits, and completions are
  posted when it asks, in batches, without interrupts. With `TASKRUN_FLAG` the kernel says when a ring has completions
  to post, so a pass enters only the rings with something to do. Each ring's completion queue (65,536) is larger than
  its connections, so a burst never spills into the kernel's overflow list.
- **Connections live in the rings, not the descriptor table.** Each ring has a table of direct descriptors as large
  as the descriptor limit (65,535 on the box), so the limit caps one ring, not the server: when a ring's table is
  within 1,024 slots of full, its accepts are cancelled and the next ring (added if needed, up to 16) takes over.
  Receives use provided buffers, so an idle connection holds no receive buffer. Eight `SO_REUSEPORT` listeners
  each have an accept queue of their own (4,096 on the box), so a ramp or a stall's new connections wait there
  instead of being refused.
- **Reading after a burst is bounded.** After a stall every connection has a request waiting, the multishot
  receives use up a ring's buffers at once, and the rest end with ENOBUFS; re-arming them all together would only
  do that again, pass after pass. So a receive that ends isn't re-armed: its connection goes on a first-in,
  first-out read queue, read with single non-blocking receives that never outnumber the ring's buffers. They
  complete inside the pass's enter, in rounds while the queue is longer than the buffers. A receive that fills its
  buffer queues the connection again; a shorter one, or an empty socket, sends it back to its multishot receive. A
  pass answers at most 2,048 requests: what it reads beyond that waits in the connection's own buffer, a copy the
  size of the request instead of a 3-4 KB socket buffer, on a ready list later passes answer first. While anything
  is queued, the loop doesn't wait.
- **One thread, proved by the compiler.** The box has one CPU, and the server is one thread: the target's default
  isolation is `MainActor`, so the server's state belongs to the main thread, and the compiler checks that. The value
  types that hold no shared state (bytes, JSON, HTTP, buffers, the store) are `nonisolated`, so their code carries
  no isolation checks. The release build keeps Swift's bounds, overflow and exclusivity checks. `main()`
  is synchronous and runs the loop: nothing is scheduled on any executor, there is no `async` on the request path,
  no locks and no atomics. Each pass submits what the last one prepared, waits on ring 0 for 256 completions or 1 ms
  (whichever comes first; with nothing happening, for anything), and takes every ring's completions.
- **The one effect that leaves it is an actor.** A checkpoint's `fsync` would stall the loop, so checkpoints are the
  `Checkpointer`'s: an actor on a thread of its own at nice 5 (on one CPU it mostly runs while the loop waits), with its
  own SQLite connection. The loop starts one without waiting for it (a `Task` with the checkpointer's executor as its
  preference), and learns it finished from an eventfd its ring reads, a completion like any other. Crossing to it is
  the one place the compiler asks for `await` and `Sendable`. The process has two threads: the loop and the
  checkpointer.
- **Checkpoints in two copies; the loop's own write starts the WAL over.** Past 4,096 frames (16 MiB) the
  checkpointer copies the WAL back while writes go on, again until at most 128 frames are left, then syncs the
  database file itself and copies once more. Then writes wait (a connection with a held write parses nothing
  further, so responses stay in order) and reads go on, while it copies what was written meanwhile; a hold lasts at
  most 250 ms. The sync before the hold is what keeps holds short: a copy that doesn't finish the WAL writes its
  pages without syncing them (SQLite syncs the file only once every frame is in), so the held copy's sync would
  flush the whole cycle while writes wait, and on a slow disk the hold would run out before it did. The database
  file grows 64 MiB at a time (one `posix_fallocate`), so a finished copy's truncate and sync don't also wait for
  the file system's journal. The checkpointer runs at nice 5: at nice 19, near saturation, it got so little of the
  one CPU that holds ran out. SQLite starts the WAL over only at a write that begins with every frame copied, which
  group commit under steady writes never leaves (locally the WAL reached 767 MB in a minute, and the WAL index SQLite
  keeps in memory with it): the pass after the hold reads with the WAL all copied, so its first write starts it over, on
  the loop's own connection. The checkpointer only copies and never commits, since a commit from another connection
  makes SQLite drop the loop connection's page cache. The log says once a minute how many holds there were, how many
  ran out, and how many copies the loop made itself (past 65,536 frames). On a runner with the server's disk capped
  at 30 MB/s and 1,000 writes a second, near saturation, holds that ran out went from most to about one in five, the
  WAL from 310 MB to 50 MB, the memory peak from 821 MB to 406 MB, the loop's own copies from 5 to 0, and p99 from
  1.4 s to 250 ms.
- **Group commit per pass.** Every request of a pass runs inside one SQLite transaction (WAL,
  `synchronous=NORMAL`), and no response of the pass is sent before its `COMMIT` returns (rule 6). If it fails, the
  pass's connections close without an answer, so no response ever claims a write that didn't land.
- **SQLite.** Opened through the `unix-excl` VFS: the process holds the file's lock for its life and the WAL index
  lives in its memory, shared by its two connections (no `-shm` file). The statements are prepared once; 64 MB
  page cache, no mmap. Automatic checkpoints are off: the loop starts PASSIVE checkpoints on the checkpointer
  (`CHECKPOINT_THREAD=0` runs them in the loop instead, past 4,096 frames). Rows are written as JSON straight from
  SQLite's memory.
- **Reads walk SQLite's B-trees.** The feed, a post, and a like's check that its post exists skip SQL: they move
  SQLite's own cursors inside the pass's transaction, as its query plan would, without the bytecode engine. SQLite's
  B-tree layer isn't part of its API, so the amalgamation is compiled with `SQLITE_PRIVATE` defined empty (it would be
  `static`): its internal functions link like any other, and `Sources/BTree` declares the ones it uses with
  `@_extern(c)`. `Cursor` moves are `mutating` and a record is a `Span` borrowed from the cursor, so the compiler
  rules out reading a row after its cursor moved on; `Record` decodes SQLite's row format in place. The feed takes its
  20 entries off `posts_created_at_id_idx`, reads the posts in ascending id order (so SQLite's seek from one post to
  the next is a step along the leaf), then their authors, counts their likes in one walk of `likes_post_id_idx`, and
  writes the response in one reservation, escaping 32 bytes at a time. That walk steps SQLite's cursor entry by entry
  (`sqlite3BtreeNext`), and its seek key is a one-column one: an `UnpackedRecord` and its `Mem`, built in the
  server's own memory at offsets for the pinned 3.54.1. At startup the server checks that seeks with it count what the
  two-column key does, and keeps the two-column key if they don't, saying so in the log. The
  build links Swift and SQLite with full link-time optimization (`build.sh`), so SQLite's small functions these reads
  call are inlined into them. The schema is checked at startup. A row these reads don't expect (a post
  without its user, a record that spills onto overflow pages) is answered by the SQL statements instead, and so is
  every read when the schema differs. Locally the answers matched SQL's byte for byte, on the seed and on databases
  edited to reach each fallback.
- **Requests parsed in place** in the kernel's receive buffers; a connection keeps bytes only when a request is
  split across receives. **Responses written in place:** the body goes straight into a send buffer taken from a
  pool, after room for the head, which is written once the body's length is known. The buffer is the connection's
  while its one send is in flight, and goes back to the pool, its capacity kept, when the send completes, so in
  steady state no response allocates. Error answers are static strings written the same way.
- **The JSON reader builds nothing.** Request bodies and token claims are checked whole and only the members the
  caller names are kept, as places in the bytes; strings are read in place and copied only when they must outlive
  the request (a new post's text, the token's username, which the response echoes).
- **Half-closed and pipelined requests.** A client that half-closes gets the answers to what it sent before the
  connection closes; pipelined requests are answered in order.
- **Shared with the author's million-users entry**: the HTTP parser, router, JSON, HS256 and buffers, written with
  `~Copyable` buffers and connections, `~Escapable` views with `@_lifetime`, `Span`/`RawSpan`/`OutputSpan` and
  `InlineArray`.

## Running

```bash
sudo ./install.sh   # Swift 6.4.2 -> /opt/swift
./build.sh          # fetches SQLite, -> .build/release/Feed
SQLITE_PATH=seed/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./start.sh
```

Optional, for testing: `RING_FILES` (connections per ring; default the descriptor limit), `MAX_RINGS` (16),
`RING_BUFFERS` (receive buffers per ring, 1024), `PASS_BATCH` (256), `PASS_WINDOW_US` (1000) and `BTREE_READS` (1; 0
reads through SQL). io_uring must be allowed: Docker's default seccomp profile blocks it.
