# fsharp-uring-delneg

| | |
|---|---|
| Language | F# on .NET 11 (SDK 11.0.100, official tarball, SHA-512 checked), ReadyToRun, workstation GC |
| Framework | none: its own io_uring layer (raw `io_uring_setup` / `io_uring_enter` / `io_uring_register` through `syscall(2)`), HTTP/1.1, JSON and JWT. No Kestrel, no ASP.NET |
| SQLite | 3.54.1 amalgamation (SHA-256 checked), unmodified, compiled by `build.sh` into `out/libsqlite3td.so` and called through P/Invoke |
| Native code of its own | none: everything but SQLite is F# |
| Nginx or direct | **direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the box) |

## Design

- **One thread drives everything** (the box has one CPU): an io_uring loop over several rings, the HTTP parser, the
  handlers and SQLite. The second thread runs WAL checkpoints on a connection of its own.
- **io_uring in F#.** `Ring.fs` maps the rings and writes submission entries in place; the rings are
  `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, so completions are posted in batches when the loop enters a ring.
  Accepted sockets are fixed (direct) descriptors from multishot accepts. A table may hold up to `RLIMIT_NOFILE`
  (65,535) entries, so the server raises its soft limit to the hard one and runs up to 8 rings: accept leaves a ring
  at a high-water mark (a quarter of its slots, at most 4,096, below full) while another ring has room, since an
  accept that finds the table full drops the connection. Eight `SO_REUSEPORT` listeners give eight accept queues
  (`TCP_DEFER_ACCEPT` on), so a ramp's new connections wait there instead of being refused.
- **Receives.** Each connection has a multishot receive into its ring's provided-buffer ring (1,024 × 1 KB), so an
  idle connection holds no buffer and a request costs one completion. A burst can empty the buffer ring and end every
  other receive with `ENOBUFS`; those connections are re-armed oldest first, at most 2,048 per ring per pass, so the
  re-arms don't empty it again. Ubuntu's 6.8 kernels refuse a buffer-ring registration with zeroed reserved words
  ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)), so a refused registration is
  retried once with `resv[0]=1`.
- **Requests are parsed in place** in the kernel's buffers; a connection keeps bytes only while a request is split
  across receives. Header search is `Span.IndexOf` (vectorized). **Responses are written in place**: the body goes
  into a pinned buffer from a pool after 128 bytes of room, the head is written in front of it once the length is
  known, and the send goes out of that buffer, so in steady state no response allocates or copies.
- **JWT (HS256).** Every token's signature, `alg` and `exp` are checked on every request. The HMAC runs on the
  system OpenSSL's SHA-256 (`libcrypto.so.3`, the library .NET itself uses), called through P/Invoke: the key's inner
  and outer padded blocks are hashed once at startup, so a token costs its own blocks and one outer block, compared
  in constant time.
- **Group commit.** Each loop pass is one SQLite transaction (WAL, `synchronous=NORMAL`), and no response of the pass
  is handed to the kernel before its `COMMIT` returns (rule 6). If a commit fails, the pass's connections close
  without an answer, so no response claims a write that didn't land. Writes commit at most once per 10 ms (reads
  aren't held), so a commit carries more writes and fewer WAL frames per write.
- **Held writes.** A write that must wait (the write window, or a checkpoint hold) moves once to a queue of its own,
  with every later request on its connection and a copy of a new post's text, and passes don't walk it again. The
  first pass after the hold answers up to 4,096 of them, oldest first, in its one commit. Releasing at most a pass's
  64 answers per write window let the held set outgrow the release rate after a checkpoint hold under load: every pass
  then walked tens of thousands of held requests, answers fell behind arrivals, and the backlog never drained.
- **Bounded passes.** A pass answers at most 64 requests; the rest stay parsed in the batch (a parsed request costs a
  few dozen bytes, one left in its socket a 3-4 KB socket buffer). Past one pass of backlog the newest requests go
  first, keeping each connection's order, and the oldest get a growing share of each pass from 8 s on, so no request
  waits without bound.
- **Checkpoints.** SQLite's automatic checkpoints are off (a WAL hook counts frames). Every 2,000 new frames the loop
  asks the checkpoint thread for a PASSIVE checkpoint and learns it finished from an eventfd its ring reads. Under
  steady writes a checkpoint never ends with the WAL fully copied, so it can't rewind: past 8,192 frames, writes wait
  (reads go on) for one more pass, and the first write after it rewinds the WAL, alone in its commit. A hold lasts at
  most 250 ms. After each pass the thread drops the database file's pages from the kernel's cache (the server reads
  through SQLite's own page cache).
- **SQLite.** Opened through the `unix-excl` VFS: the process holds the file's lock for its life and the WAL index
  lives in its memory, shared by its two connections. 256 MB page cache, no mmap, statements prepared once. Startup
  reads the indexes that writes touch into the page cache. Inserts bind `created_at` in the column default's format
  and take the id from `sqlite3_last_insert_rowid` (no `RETURNING`); a like is one `INSERT … SELECT … WHERE EXISTS
  … ON CONFLICT DO NOTHING`, and `sqlite3_changes` tells a new like from a repeat or a missing post.

## Reads walk SQLite's B-trees

`GET /feed` and `GET /posts/:id` read SQLite's own B-tree cursors inside the pass's transaction, without the bytecode
engine (allowed: LEADERBOARD.md, Arjay's rulings on SQLite). SQLite's B-tree functions aren't part of its API: the
amalgamation declares them `SQLITE_PRIVATE`, which is `static` unless defined, so `build.sh` compiles it with
`-DSQLITE_PRIVATE=` and they are exported (`-fno-semantic-interposition -Wl,-Bsymbolic` keep SQLite's own calls among
them direct). `Sqlite.fs` declares the ones the reads use. The server never writes SQLite's state: it opens, moves and
closes cursors, decodes the record bytes SQLite returns (`Db.fs`), and builds only its own search key (an
`UnpackedRecord` and a `Mem` in its own memory, at the pinned 3.54.1's offsets) for `sqlite3BtreeIndexMoveto`.

- **Feed:** the first 20 entries of `posts_created_at_id_idx` (`created_at DESC, id DESC`, so exactly the spec's
  order), then each post and its author. The n-th post and its author are read through cursors of their own, so when
  requests in a pass ask for the same rows, SQLite's move returns at once from a cursor already there; every request
  still decodes the rows from SQLite's pages. Like counts take one range walk of `likes_post_id_idx` when the 20 ids
  lie within 512 of each other, a seek per post otherwise.
- **Live reads:** every request reads its rows while it is served (rule 5). Only prepared statements, cursors, key
  descriptions and the four root pages are kept, and the schema cookie is checked on every read.
- **Fallbacks to SQL:** a row the decoder doesn't cover (a record spilling onto overflow pages, an unexpected type, a
  post without its user) is read with the reference SQL query. At startup the server checks the column order of each
  table and index it decodes, then compares the B-tree answers with SQL's, byte for byte, on the feed and on the
  posts it lists: on any difference every read uses SQL. `BTREE=0` turns the B-tree reads off.

## Files

| | |
|---|---|
| `src/Ring.fs` | io_uring: setup, mmap, submission entries, completions |
| `src/Server.fs` | the loop: rings, accept, receive, send, close, HTTP, the batch and its transaction, held writes, checkpoint holds |
| `src/Db.fs` | statements, writes, record decoding, the feed and post reads, the self-test, the checkpoint thread |
| `src/Sqlite.fs` | P/Invoke declarations for SQLite's API and its B-tree functions |
| `src/Json.fs`, `src/Jwt.fs` | response JSON, HS256 |
| `src/Native.fs`, `src/Program.fs` | libc calls, startup |

SQLite build options (`build.sh`): `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`,
`LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `TEMP_STORE=3`, `USE_ALLOCA`, `OMIT_DEPRECATED`,
`OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`,
`OMIT_COMPLETE`, `OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`; `-O3 -march=native`.

## Run

```bash
sudo ./install.sh   # .NET SDK 11.0.100 -> /usr/local/dotnet, gcc, unzip
./build.sh          # SQLite -> out/libsqlite3td.so, the server -> out/Server
SQLITE_PATH=seed/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./start.sh
```

io_uring must be allowed (Docker's default seccomp profile blocks it). Optional env vars, for testing: `RINGS` (8),
`LISTENERS` (8), `ANSWERS` (64), `WWIN` (ms, 10), `BUFS` (1024), `CACHE_KB` (256000), `BTREE` (1), `VFS`
(`unix-excl`), `STATS` (1: a line a second on stderr).

## Credits

The loop's policies (accept high-water marks, bounded re-arms, answers per pass, newest-first under backlog, the write
window, checkpoint holds) come from the author's [objc-uring-delneg](../objc-uring-delneg), and `Ring.fs` and `Jwt.fs`
from the author's F# entries. Ideas from other entries: the `unix-excl` VFS and eight `SO_REUSEPORT` listeners from
[rust-iouring-kensac](../rust-iouring-kensac), and calling SQLite's `sqlite3Btree*` functions on the unmodified
amalgamation built with `SQLITE_PRIVATE` defined empty from [swift-iouring-akbashev](../swift-iouring-akbashev).

MIT license (the repository's).
