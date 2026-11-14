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
  handlers and SQLite. The second thread runs WAL checkpoints on a connection of its own, at nice 19, so it takes
  the CPU only when the loop leaves it.
- **io_uring in F#.** `Ring.fs` maps the rings and writes submission entries in place; the rings are
  `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, so completions are posted in batches when the loop enters a ring.
  Accepted sockets are fixed (direct) descriptors from multishot accepts. A table may hold up to `RLIMIT_NOFILE`
  (65,535) entries, so the server raises its soft limit to the hard one and runs up to 8 rings: accept leaves a ring
  at a high-water mark (a quarter of its slots, at most 4,096, below full) while another ring has room, since an
  accept that finds the table full drops the connection. Eight `SO_REUSEPORT` listeners give eight accept queues
  (see admission below). A connection is accepted when its handshake completes (`DEFACC=1` sets `TCP_DEFER_ACCEPT`,
  which keeps it out of the queue until its first request arrives).
- **Admission at the door.** Each listener's accept queue holds 64 connections (`ADMITQ`). While the oldest request
  waiting for an answer is 50 ms old or more, the loop cancels its accepts, and it accepts again once that age is
  under 10 ms, so the connections already open get the CPU through a burst of arrivals. Meanwhile new connections
  wait in the accept queues; once those are full the kernel drops their SYNs and the client sends them again (after
  1 s, then 3 s). Part of the gain comes from where that wait is counted: a load generator that times a request from
  the moment its connection is up (mload leaves the dial out) doesn't count a resent SYN as request time. The idea,
  and this note, come from [c-iouring-jowinjestine](../c-iouring-jowinjestine) (#435).
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
  without an answer, so no response claims a write that didn't land. Writes commit at most once per 20 ms (reads
  aren't held), so a commit carries more writes and fewer WAL frames per write. A pass that only reads doesn't
  commit: the next pass goes on in its read transaction, with its cursors where they are, so most passes skip
  `BEGIN`/`COMMIT` and the cursors' descents from the root. It ends once the WAL is all copied, so the next write
  can rewind it.
- **Held writes.** A write that must wait (the write window, or a checkpoint hold) moves once to a queue of its own,
  with every later request on its connection and a copy of a new post's text, and passes don't walk it again. The
  first pass after the hold answers up to 4,096 of them, oldest first, in its one commit. Releasing at most a pass's
  64 answers per write window let the held set outgrow the release rate after a checkpoint hold under load: every pass
  then walked tens of thousands of held requests, answers fell behind arrivals, and the backlog never drained.
- **Bounded passes.** A pass answers at most 64 requests; the rest stay parsed in the batch (a parsed request costs a
  few dozen bytes, one left in its socket a 3-4 KB socket buffer). Past one pass of backlog the newest requests go
  first, keeping each connection's order, and the oldest get a growing share of each pass from 8 s on, so no request
  waits without bound.
- **Checkpoints.** SQLite's automatic checkpoints are off (a WAL hook counts frames). Every 4,000 new frames the loop
  asks the checkpoint thread for a PASSIVE checkpoint and learns it finished from an eventfd its ring reads. Under
  steady writes a checkpoint never ends with the WAL fully copied, so it can't rewind: past 16,384 frames, writes wait
  (reads go on) for one more pass, and the first write after it rewinds the WAL, alone in its commit. A hold lasts at
  most 250 ms, with one exception. Late in a long run a pass can take a second or more on the disk. A hold that times out
  lets writes in, but the next hold starts at once on the same pass, and the writes it lets in keep the WAL from
  rewinding when that pass ends. Without the exception, no hold would complete and the WAL would grow. So the hold that
  starts on a pass an earlier hold already timed out on waits up to 1.5 s for it (`HOLDRIDE`), and the pass after it
  gets the usual 250 ms. After each pass the thread drops the WAL's copied frames from the kernel's cache (no reader goes back to
  them), so a WAL that grows while the disk is slow doesn't fill memory. The database file's pages go too (the server
  reads through SQLite's own page cache), but only after the pass that syncs the file: SQLite syncs it only at the end
  of a pass that copied the whole WAL with no commit coming in, i.e. a held pass. Dropping after the other passes
  started writeback of every page each of them wrote, a hot index page once per pass; left dirty until that sync, each
  page is written once, and the disk got about 60 % fewer write requests per request on the test box.
- **SQLite.** Opened through the `unix-excl` VFS: the process holds the file's lock for its life and the WAL index
  lives in its memory, shared by its two connections. 256 MB page cache, no mmap, statements prepared once. Startup
  reads the indexes that writes touch into the page cache. Inserts bind `created_at` in the column default's format
  and take the id from `sqlite3_last_insert_rowid` (no `RETURNING`); a like is one `INSERT … SELECT … WHERE EXISTS
  … ON CONFLICT DO NOTHING`, and `sqlite3_changes` tells a new like from a repeat or a missing post.
- **One write per commit.** A VFS of its own over `unix-excl` (SQLite's public VFS interface, the pattern of its own
  vfstrace and multiplex shims) changes only the WAL file's writes. SQLite writes a transaction's frames one after
  another, each as two writes (its 24-byte header, then the page): thousands of `pwrite` calls a second under load.
  While the loop runs `COMMIT`, writes that continue where the last one ended go into a buffer, which goes to the file
  when the commit frame's page arrives (its header's database-size field is non-zero), inside SQLite's own call. So the
  commit is in the kernel before SQLite publishes it in the WAL index and before `COMMIT` returns, and the buffer is
  empty whenever SQLite isn't inside that call. Any other write, and every sync, truncate and close, writes the buffer
  first. The unix VFS hands at most 131,071 bytes to one `pwrite` (and reports a longer write as a full disk), so a
  large commit goes out in pieces that size. Reads, locks and the database file are `unix-excl`'s.
- **Page cache.** SQLite's own page cache (pcache1) through `SQLITE_CONFIG_PCACHE2`, with one method of its own,
  `xRekey`. A B-tree split that puts its new pages in order swaps two of them through a temporary page number, the
  lock-byte page's (262,145 for 4 KB pages), and pcache1 keeps the largest page number it has held until a truncate
  walks the cache. So after each such swap, the next commit's truncate (drop the pages past the end of the file)
  scanned the whole hash table, tens of thousands of slots, to find nothing: 2.5 % of the server's CPU under a stream
  of likes. Here the page waits out the swap under page number 0, which SQLite never gives a page, and that walk is
  gone. Fetches, truncates and everything else are pcache1's; `xRekey` runs only inside an `INSERT`'s split.

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
  descriptions and the four root pages are kept, and the schema cookie is checked on every read. A read transaction
  that read-only passes share always sees the latest commit: only the loop writes, and a pass that writes commits.
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
| `src/Vfs.fs` | the VFS that writes each commit's WAL frames with one `pwrite` |
| `src/Pcache.fs` | pcache1 with an `xRekey` of its own |
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
`LISTENERS` (8), `ANSWERS` (64), `WWIN` (ms, 20), `HOLDRIDE` (ms, 1500), `ADMITQ` (64; 0: no admission control),
`BUFS` (1024), `CACHE_KB` (256000), `BTREE` (1), `VFS` (`unix-excl`), `WALBUF` (1; 0: WAL frames written one by one),
`PCACHE` (1; 0: pcache1 as it is), `STATS` (1: a line a second on stderr).

## Credits

The loop's policies (accept high-water marks, bounded re-arms, answers per pass, newest-first under backlog, the write
window, checkpoint holds) come from the author's [objc-uring-delneg](../objc-uring-delneg), and `Ring.fs` and `Jwt.fs`
from the author's F# entries. Ideas from other entries: the `unix-excl` VFS and eight `SO_REUSEPORT` listeners from
[rust-iouring-kensac](../rust-iouring-kensac), and calling SQLite's `sqlite3Btree*` functions on the unmodified
amalgamation built with `SQLITE_PRIVATE` defined empty from [swift-iouring-akbashev](../swift-iouring-akbashev),
and writing each commit's WAL frames with one `pwrite` at the commit frame, and the page cache's truncate walk, from
[c-iouring-pavlyhalim](../c-iouring-pavlyhalim).

MIT license (the repository's).
