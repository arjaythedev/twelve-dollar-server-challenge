# Rust, no framework: io_uring + SQLite B-tree reads

One thread drives everything: an io_uring event loop, an HTTP/1.1 parser, a JSON reader/writer,
HS256 verification, and SQLite (the pinned, unmodified amalgamation, linked in statically).

| | |
|---|---|
| Language | Rust 1.94.0 (official standalone tarball, sha256-checked by `install.sh`) |
| Crates | `io-uring` 0.7.15, `libc` 0.2.183, `sha2` 0.10.9, `memchr` 2.8.0, `cc` 1.2.56 (build); pinned in `Cargo.lock` |
| SQLite | 3.54.1 amalgamation, sha256-checked, compiled by `build.rs` with `-O3 -march=native` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings). There is no epoll fallback, so it won't start where io_uring is blocked (e.g. Docker's default seccomp profile). Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843), an inverted check), so a refused registration is retried once with `resv[0]=1` |

## Reads go through SQLite's B-tree, not its SQL engine

`GET /feed` and `GET /posts/:id` read SQLite's own B-tree cursors
(`csrc/shim.c` `#include`s the unmodified amalgamation), inside the pass's SQLite transaction. Rust then
decodes the record bytes SQLite returns and writes the JSON directly. There's no bytecode engine,
no `sqlite3_column_*` calls, and no copying through SQLite values. This is the same approach as
c-uring-curvynes #217, rust-native #222 and ada-native #223: an unmodified amalgamation, writes
through SQL, and a SQL fallback. It is still pending a rules ruling (rules 2/3). Unlike
mojo-iouring #224, it keeps no state about the data (no "dense ids" flag, no last id). Unlike
c-iouring-public #158, it doesn't patch SQLite's source.

- **Feed:** walk `posts_created_at_id_idx` from its first entry. The index is
  `(created_at DESC, id DESC)`, so this is exactly `ORDER BY created_at DESC, id DESC`, and its
  records already hold `created_at` and `id`. The 20 posts are then looked up in ascending id
  order. Consecutive ids land on the next cell of the same leaf (SQLite's own `TableMoveto`
  fast path), so no root-to-leaf descent is needed per row. Next come the 20 authors in `users`.
  The like counts take one range walk of `likes_post_id_idx` when the ids fall within 512 of each
  other, and a seek per post otherwise.
- **No assumptions about the data:** the plan doesn't need dense ids, timestamps in id order, or
  any remembered state. If a post has no user row (where the SQL `JOIN` would drop it), the
  reference SQL query answers instead. At startup the server checks the column order of every
  table and index it decodes, and refuses to start on a different schema.
- **Live reads:** every request reads its rows from SQLite while it is served (rule 5).
  Only prepared statements, cursors, KeyInfo objects and the four B-tree root page numbers
  (read from `sqlite_schema` at startup) are reused. `locking_mode=EXCLUSIVE` means no other
  connection can change the schema while the server runs.
- **Writes use SQL:** `INSERT … RETURNING id, created_at` for posts, `INSERT … ON CONFLICT DO
  NOTHING` for likes. Only `sqlite3_changes` tells a new like from a repeat. The like handler's
  check that the post exists (the 404 case) is a B-tree seek on `posts`, like the reads.

## Durability and batching

- WAL with `synchronous=NORMAL` and `locking_mode=EXCLUSIVE`. Only this process uses the file,
  so WAL needs no shared-memory index or file locks.
- Each event-loop pass is one SQLite transaction. Its reads share a snapshot, and its writes are
  group-committed (rule 6). **No response from a pass is handed to the kernel until its COMMIT
  returns.** If a commit fails, every response from that pass is replaced with a 500 and the
  connection is closed, so a 201 never claims a write that didn't land. Reads that see this
  pass's own writes are also held until COMMIT, and become 500s if it fails, so no client ever
  sees a row that wasn't committed. If SQLite rolls the transaction back by itself mid-pass
  (`SQLITE_FULL`, I/O errors), the rest of the pass doesn't write, and its COMMIT reports the
  failure.
- Automatic checkpoints are off. After a commit leaves 8,192 or more WAL frames (about 32 MB),
  a PASSIVE checkpoint copies the log back. With no other readers it always completes, so the
  next write restarts the log. It runs on the event-loop thread right after a COMMIT, so the
  responses of that one pass wait for it (a latency spike every ~8k frames). A longer log
  means fewer fsyncs, and pages written many times (the newest posts and likes leaves) are
  copied back once per checkpoint.
- No `mmap`: reads go through SQLite's page cache (64 MB). A cached page keeps its parsed
  B-tree header and is found without a WAL-index lookup; an mmap page pays both on every fetch.

## Connections

Every k6 user holds a keep-alive connection, so a high score means hundreds of thousands of
sockets under `LimitNOFILE=65535`.

- Accepted sockets are io_uring **direct descriptors**. They live in a ring's file table, not the
  process fd table. A table can be no larger than `RLIMIT_NOFILE`, so the server raises its soft
  limit to the hard one and gives each ring that many slots.
- It starts with one ring. When a ring is within a margin of full (1/8 of its slots, between 65
  and 512), accepting moves to the next ring, adding one if needed (up to 16). Accepting moves
  before the table is actually full because an accept that finds no free slot has already taken
  the connection off the listen queue, and the kernel would drop it. Accepts are single-shot,
  at most 64 in flight, so no more than that can land after the switch. A ring accepts again
  once more than twice the margin of its slots are free. If a ring can't be added, adding is
  retried once a second.
- The port has eight `SO_REUSEPORT` listeners, each with 8 of the 64 accepts. A listen queue is
  capped at `net.core.somaxconn` (4,096 by default), so eight queues hold 32k pending connections
  through a ramp or a slow pass instead of refusing them.
- One thread drives every ring (`SINGLE_ISSUER`, `DEFER_TASKRUN`). Each pass first gives every
  other ring one non-blocking `io_uring_enter` that submits its queued sends and collects its
  completions. Then ring 0 submits and waits for 256 completions or 1 ms, whichever comes
  first. So a pass never spins, and bigger passes share one COMMIT. The wait adds about 1 ms of
  latency on ring 0, and up to about 2 ms on the other rings.
- Under `LimitNOFILE=65535`, every ring preallocates a 65,535-entry table of 40-byte connection
  records (2.6 MB) and 2 MiB of receive buffers: ~6 MB of user space per ring, ~30 MB for five
  rings (300k connections). The table size follows the hard descriptor limit. The pool of
  output buffers adds at most 2,048 × 32 KB. Kernel socket memory (~3.4 KB per connection
  measured) comes on top of all this.
- Each connection has one **multishot receive** into a provided-buffer ring (1,024 × 2 KB per
  ring), so an idle connection holds no receive buffer. Receives that run out of buffers are
  re-armed after the pass has returned its buffers.
- Output buffers come from a pool and go back once a send completes, so idle connections hold
  no heap memory, only a fixed 40-byte record (its three buffers are null pointers until
  used). One send per connection is in flight at a time. Pipelined responses queue behind it.
- Closing is a `SHUTDOWN` hard-linked to a fixed-file `CLOSE`. The shutdown ends the multishot
  receive. A generation number in every `user_data` drops completions for a reused slot.
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
| `src/app.rs` | routes, handlers, responses |
| `src/db.rs` | record decoding, the feed and post plans, writes (FFI to the shim) |
| `src/http.rs`, `src/json.rs`, `src/jwt.rs` | request framing, JSON, HS256 |
| `csrc/shim.c` | the amalgamation plus B-tree cursor and statement helpers |
| `build.rs` | compiles the shim and SQLite with the options below |

SQLite options (all in `build.rs`): `THREADSAFE=0` (one thread), `DEFAULT_MEMSTATUS=0`,
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
