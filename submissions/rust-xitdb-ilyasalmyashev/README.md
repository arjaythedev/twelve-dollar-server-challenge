# Rust, no framework: io_uring + xitdb

One thread drives everything: an io_uring event loop, an HTTP/1.1 parser, a JSON reader/writer,
HS256 verification, and the database, [xitdb](https://github.com/ToyVo/xitdb-rs). The one exception
is `fdatasync`, which a second thread runs.

**This entry does not use SQLite, so it breaks rule 2 on purpose.** SQLite's file at `$SQLITE_PATH`
is only the place the seed comes from: it's read once, by a small reader of its file format, and
never touched again. Nothing of SQLite is linked in. All the other rules hold.

| | |
|---|---|
| Language | Rust 1.94.0 (official standalone tarball, sha256-checked by `install.sh`) |
| Crates | `xitdb` 0.2.0 (git, pinned revision), `io-uring` 0.7.15, `libc` 0.2.183, `sha2` 0.10.9, `memchr` 2.8.0; pinned in `Cargo.lock` |
| Database | xitdb, in the file `$SQLITE_PATH.xitdb` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings). There is no epoll fallback, so it won't start where io_uring is blocked (e.g. Docker's default seccomp profile). Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843), an inverted check), so a refused registration is retried once with `resv[0]=1` |

## The data on xitdb

xitdb has no tables or query engine: a database is one tree of nested collections, and every commit
is a new immutable root. The root here is a list of four collections:

| SQLite | xitdb | |
|---|---|---|
| `users` | `ArrayList`, `users[id-1]` | bytes: `created_at`, `username` |
| `posts` | `ArrayList`, `posts[id-1]` | bytes: `user_id`, `created_at`, `body` |
| `likes` and `likes_post_id_idx` | `ArrayList` of one `CountedHashMap` per post | user id to `created_at` in ms |
| `posts_created_at_id_idx` | `ArrayList`, the timeline | post ids in `(created_at, id)` order |

- **`like_count` is the map's size.** A counted map keeps its entry count in its header, so counting
  is one read, and the count can't disagree with the rows: it *is* the likes of that post. One like
  per (user, post) is the map's key.
- **The feed is the end of the timeline.** A new post is the newest by construction, so an append
  keeps the list in index order. So that a clock stepping back can't break that, a post is never
  dated before the newest one (the column is server-assigned, and equal times sort by id). Nothing
  assumes ids are in time order: the import sorts the seed by `(created_at, id)`.
- **Ids can have gaps.** A gap is an empty slot, which reads as a missing row.
- **Map keys aren't hashed with SHA-1.** xitdb's hasher is pluggable, and the keys are integers, so
  the "hash" is a bijective 64-bit mix: 8 bytes per entry instead of 20, and no collisions.
- A post whose user doesn't exist is treated as the SQL join treats it: it isn't there. All the seed's
  rows are carried over, including `created_at` of users and likes, which no endpoint reads.

## Startup

At the first start the seed goes into `$SQLITE_PATH.xitdb`: 1.2 s and 255 MB (SQLite's file is 238 MB)
on the machine in the results below. The import writes to a temporary file, and renames it once it's
synced. A restart finds the file, and carries on with it. The file records the seed's size and mtime,
so a fresh copy of the seed is imported again, rather than served from older data. A nonempty `-wal`
next to the seed is refused, because this reader only sees the main file.

## Reads go through xitdb's format, not its cursors

`GET /feed` and `GET /posts/:id` walk xitdb's file format directly, in one read-only mapping of the
file (`Shared` in `src/store.rs`). A list lookup through the crate's `ArrayList` costs about 130 ns:
a `RefCell`, a seek, a copy and checked arithmetic at each of its 5 or 6 tiers. A feed takes 80 of those,
so it spent 10.7 us there. Walking the same blocks takes about 2 us for the whole feed, JSON included,
and strings are read in place, with no copy and no allocation. The format is stable and shared by every
xitdb implementation (Zig, Go, Java, ...), and each step is bounds-checked. Everything else goes through
the crate: all writes and commits, and checking whether a like exists.

- **Live reads (rule 5):** every request walks the file while it's served. All that's kept between
  requests is `View`, the file positions of the four lists in one commit. Commits are immutable, so
  those positions can't go stale, and they don't copy any data.
- **Memory:** there is no page cache of our own. Anonymous memory is ~9 MB, against ~69 MB for the
  SQLite version with its 64 MB cache. The mapping's pages are the kernel's page cache, shown in RSS
  but evictable.

## Writes and durability

xitdb's transaction is a closure, so it can't stay open across requests. And `Database`'s commit does
**not** sync the root header it rewrites last, so even with its `fsync` option on, the latest commit
isn't durable. Both are handled outside the crate, through its `Core` trait:

- **A handler only validates.** It checks auth and the body, answers what needs no write (errors, a
  repeated like, a missing post), and queues the rest. That connection then stops: what it sent
  behind the write stays in its buffer, so its answers keep their order.
- **One transaction per sync round.** When no round is running, every queued write is committed as
  one transaction, in a single `pwrite`. While a round runs, writes wait: their answers couldn't leave
  before the next round anyway. A batch costs ~25 us plus ~3 us per write.
- **The root header stays in memory** until the data it points to is on disk. The second thread
  runs `fdatasync`, writes the header, and runs `fdatasync` again. The header on disk therefore never
  points at anything that isn't on disk, and whatever is behind it after a crash is written over.
- **Only then do the answers go out**, so a 2xx means the write is on disk. That's stronger than
  rule 6 asks: SQLite's WAL with `synchronous=NORMAL` doesn't sync at commit, so it can lose its last
  commits in a power cut.
- **Reads stay on the last durable commit,** so nobody sees a row that could still vanish. They move
  on when the round ends, before its answers go out, so a client always reads its own write.
- **Rounds start at least 5 ms apart**, so under load more writes share each fsync. Throughput rose
  20-40% against no gap, with no worse write latency, because fewer fsyncs fight for the disk. A lone
  write on a quiet server starts a round at once.
- **The file is kept 4 MiB ahead of the commits, in real zeros.** A commit that grows a file makes
  `fdatasync` commit the filesystem's journal, and with `fallocate`'s unwritten extents it does so too.
  Overwriting blocks already written needs only the data and a cache flush: write p50 fell 20% and
  throughput rose 5-13%, where `fallocate` made no reliable difference. Larger steps cost a slower
  round each (32 MB: 60 ms). So the file's length means nothing: the size is the root header's.
- `pwrite` never races `fdatasync`: nothing is written while a round runs. So the event loop can't
  stall on the filesystem's journal, and it never waits for a disk.
- If a commit fails, its writes get a 500 and their connections close. If `fdatasync` fails the server
  exits, because the kernel may have dropped the pages, and retrying can falsely succeed. Nothing
  unsynced was acknowledged, so a restart loses nothing it promised.
- **The file only grows.** Every commit copies the path to what it changed (~900 B per write on k6's
  mix, about what SQLite's log takes), and xitdb keeps old versions. There's no compaction: xitdb's needs a
  stopped writer. Superseded blocks are simply never read, so the kernel evicts them.

## Results

**Throughput and latency were measured with a different network layer, and not on the real VM.** This
server needs Linux 6.1+, and the machine used has a 5.15 kernel. So the same `App` and `Db` code ran
behind a small epoll front end, against the SQLite version of this server behind the same front end,
on one pinned CPU of a 22-thread desktop under WSL2, on ext4 in a virtual disk. Loopback TCP dominates
the CPU there, so the differences are real, but the absolute numbers say nothing about the droplet,
and this entry has no benchmark score yet. Closed loop, 200 connections, medians of 5 interleaved
10 s runs, 0 errors:

| | SQLite req/s | xitdb req/s | | CPU per request | read p99 | write p50 | write p99 | write max |
|---|---|---|---|---|---|---|---|---|
| reads only | 100,756 | **143,146** | +42% | 10.1 → 7.3 us | 3.72 → 2.84 ms | | | |
| k6's mix (8% writes) | 70,282 | **75,180** | +7% | 13.3 → 13.6 us | 5.81 → 4.84 ms | 2.44 → 6.52 ms | 94 → 12.5 ms | 111 → 20 ms |
| write-heavy (35%) | 43,767 | **66,966** | +53% | 18.4 → 14.2 us | 9.60 → 3.40 ms | 3.14 → 5.83 ms | 93 → 12 ms | 109 → 18 ms |

| In-process, one thread | SQLite | xitdb |
|---|---|---|
| `GET /feed` | 4.8 us | 2.3 us |
| `GET /posts/:id` | 3.6 us | 1.2 us |
| Anonymous memory | 69 MB | 9 MB |
| User-space CPU, k6's mix, 12 s | 4.8 s | 2.5 s |

`bench/load.js` with k6 at 15,000 VUs, on that same setup (1,130 req/s, not near the limit): both pass
every threshold with 0 failures, on 41% less server CPU here (10 s against 17 s). Latency is the cost
of syncing: **the median is about the same (253 us against 285 us), but p95 is 3.1 ms against 0.5 ms and
p99 7.2 ms against 0.9 ms**, because the slowest 8% of requests are the writes, and those wait for a
disk. Under heavier load this reverses: the p99 of writes above is 7.5 times better, since there's no
checkpoint to stall them.

**The real server** ran on Ubuntu 24.04's `6.8.0-147-generic` in a VM, under software emulation, so
for correctness only. `test/run.sh` passes 42/42 there. `bench/load.js` with 300 VUs passes, with 1,872
of 1,872 checks. The durability test ran the load, kept a log of every write answered with a 2xx, crashed
the server, restarted it, and checked each one:

| | acknowledged | lost |
|---|---|---|
| `kill -9` of the server | 15,666 | **0** |
| Power cut (the VM killed from outside): first run | 7,163 | **0** |
| Power cut: second run | 4,090 | **0** |
| Control: power cut with `fdatasync` disabled | 13,226 | 13,225 |

The control is what makes the zeros mean something: it shows the test would notice. It's a guest
crash, though, not a disk losing its cache: the emulated disk kept what the guest had flushed.

## Connections

Every k6 user holds a keep-alive connection, so a high score means hundreds of thousands of
sockets under `LimitNOFILE=65535`.

- Accepted sockets are io_uring **direct descriptors**. They live in a ring's file table, not the
  process fd table. A table can be no larger than `RLIMIT_NOFILE`, so the server raises its soft
  limit to the hard one and gives each ring that many slots.
- It starts with one ring. When a ring is within a margin of full (1/8 of its slots, up to
  1,024), accepting moves to the next ring, adding one if needed (up to 16). Accepting moves
  before the table is actually full because an accept that finds no free slot has already taken
  the connection off the listen queue, and the kernel would drop it. Accepts are single-shot,
  at most 512 in flight (a sixteenth of a small table), always fewer than the margin, so no
  more than that can land after the switch. A ring accepts again once more than twice the
  margin of its slots are free. If a ring can't be added, adding is retried once a second.
- An accept is re-armed only after a pass sees it complete, so the number in flight bounds the
  accept rate. With 64, a ramp of a few thousand new connections a second outran it on a 1 vCPU
  droplet: the listen queue filled and new connections waited out SYN retransmits (1 s, 3 s,
  7 s). 512 keeps up.
- The port has eight `SO_REUSEPORT` listeners, each with an eighth of the accepts. A listen
  queue is capped at `net.core.somaxconn` (4,096 by default), so eight queues hold 32k pending
  connections through a ramp or a slow pass instead of refusing them.
- One thread drives every ring (`SINGLE_ISSUER`, `DEFER_TASKRUN`). Each pass first gives every
  other ring one non-blocking `io_uring_enter` that submits its queued sends and collects its
  completions. Then ring 0 submits and waits for 256 completions or 1 ms, whichever comes
  first. So a pass never spins. The wait adds about 1 ms of latency on ring 0, and up to about
  2 ms on the other rings.
- Under `LimitNOFILE=65535`, every ring preallocates a 65,535-entry table of 40-byte connection
  records (2.6 MB) and 2 MiB of receive buffers: ~5 MB of user space per ring, ~25 MB for five
  rings (300k connections). The table size follows the hard descriptor limit. The pool of
  output buffers adds at most 2,048 × 32 KB. Kernel socket memory (~3.4 KB per connection
  measured) comes on top of all this.
- Each connection has one **multishot receive** into a provided-buffer ring (1,024 × 2 KB per
  ring), so an idle connection holds no receive buffer, and a request costs one completion.
- **After a burst, reads are bounded.** After a stall, every connection has a request waiting,
  and the multishot receives use up the buffer ring at once. The rest end with ENOBUFS. Re-arming
  them all together would only repeat that, pass after pass. So a receive that ends is not
  re-armed. Its connection goes on a first-in, first-out read queue instead. Each pass then
  issues one single-shot receive per queued connection, with `MSG_DONTWAIT`, and never more on a
  ring than it has buffers. The reads are submitted and reaped in the same pass (with
  `DEFER_TASKRUN` they complete inside the non-blocking enter), in rounds while the queue is
  longer than the buffers. A read that fills its buffer is queued again. A shorter read, or
  EAGAIN, means the socket is drained, and the multishot receive is armed again.
- **Drain first.** A pass reads up to 16,384 times but answers at most 2,048 reads. The rest are
  kept on the connection, next to a partial request, and go on a ready list that later passes
  answer first, oldest first. While anything is queued, ring 0 doesn't wait. Reading costs a
  copy the size of the request, while a request left in its socket costs a 3–4 KB socket buffer.
  So the socket queues empty as fast as the reads can run, instead of building up toward TCP's
  memory limit. In a 40k-user run with four 2 s stalls, receives ended with ENOBUFS about once
  per connection per stall (119k in all), where re-arming them all at once had caused 2.15
  million. Half a second after each stall, TCP held ~600 of its ~29k pages, where it had held
  ~20k before.
  [c-iouring-pavlyhalim](../c-iouring-pavlyhalim) (#245) reads this way all the time, from a
  readiness poll. Here that costs ~10% more CPU per request in steady state (a poll completion
  and a read for each request), so it is used only to recover from a burst.
- Output buffers come from a pool and go back once a send completes, so idle connections hold
  no heap memory, only a fixed 40-byte record (its three buffers are null pointers until
  used). One send per connection is in flight at a time. Pipelined responses queue behind it.
- Closing is a `SHUTDOWN` hard-linked to a fixed-file `CLOSE`. The shutdown ends the multishot
  receive or the single-shot read in flight. A generation number in every `user_data` drops completions for a reused slot.
- Idle connections are never closed by the server (Nginx-style 65 s reuse is safe).

## Request path

- **HTTP:** one pass over the head's lines finds the request line, `Authorization` and
  `Content-Length`. A request split across receives is kept per connection until it completes.
  Heads over 16 KB, bodies over 1 MB, chunked bodies and malformed request lines get
  400 `{"error":"bad request"}` and the connection is closed (out of scope in SPEC).
- **JWT:** the HMAC key's inner and outer padded blocks are hashed once at startup. Each
  request hashes only `header.payload` plus the outer block, and compares the result in constant
  time. Every token's signature, `alg` (HS256 only) and `exp` (required) are checked on every
  request. Only the canonical base64url spelling of a signature is accepted.
- **JSON:** a validating reader checks whole request bodies and reports the top-level members,
  allocating only for escaped strings. The last duplicate key wins and escaped member names
  are decoded, as in `JSON.parse`. Escaped strings are decoded once. Output strings are scanned 8 bytes at a time for characters that need escaping,
  and copied in one piece when none do. `Content-Length` is back-filled.
- `body` is trimmed with JavaScript's `trim()` whitespace set and counted in characters (code points,
  the same unit as the schema's `CHECK (length(body) …)`).

## Files

| | |
|---|---|
| `src/main.rs` | rings, accept/receive/send/close, the batch loop |
| `src/app.rs` | routes, handlers, responses, the commit of a pass |
| `src/db.rs` | the data model, reads, the transaction |
| `src/store.rs` | xitdb's storage backend, the walk over its format, the sync thread |
| `src/import.rs` | the seed: a reader of SQLite's file format, and the import |
| `src/http.rs`, `src/json.rs`, `src/jwt.rs` | request framing, JSON, HS256 |

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
