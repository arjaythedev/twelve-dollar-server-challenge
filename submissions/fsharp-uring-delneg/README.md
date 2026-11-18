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
  handlers and SQLite. The second thread runs WAL checkpoints on a connection of its own, at nice 19 (`CKNICE`):
  it gets ~1 % of the CPU while the loop has requests queued. At the end of a ramp a pass then takes seconds instead
  of 15-30 ms, which used to make write holds time out; writes no longer wait for checkpoints (see below), so a slow
  pass now delays nothing, and the CPU that nice 10 gave the thread at the busiest moment (up to ~10 %) stays with
  the loop.
- **io_uring in F#.** `Ring.fs` maps the rings and writes submission entries in place; the rings are
  `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, so completions are posted in batches when the loop enters a ring.
  Accepted sockets are fixed (direct) descriptors from multishot accepts. A table may hold up to `RLIMIT_NOFILE`
  (65,535) entries, so the server raises its soft limit to the hard one and runs up to 8 rings: accept leaves a ring
  at a high-water mark (a quarter of its slots, at most 4,096, below full) while another ring has room, since an
  accept that finds the table full drops the connection. Eight `SO_REUSEPORT` listeners give eight accept queues,
  so a ramp's new connections wait there instead of being refused. A connection is accepted when its handshake
  completes (`DEFACC=1` sets `TCP_DEFER_ACCEPT`, which keeps it out of the queue until its first request arrives). The
  listeners have `TCP_QUICKACK` off, and accepted sockets inherit it, so a connection's first request is acknowledged
  by its response instead of by a bare ACK of its own (`QUICKACK=1` restores the kernel default).
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
- **Checkpoints, and writes that don't wait for them.** SQLite's automatic checkpoints are off (a WAL hook counts
  frames). Every 16,000 new frames the loop asks the checkpoint thread for a PASSIVE checkpoint and learns it
  finished from an eventfd its ring reads. Under steady writes a pass never ends with the WAL fully copied, so the WAL
  can't rewind unless writes stop for a pass. They used to: past 16,384 frames (64 MB), about every 3 s at 300k users,
  writes waited (reads went on) until a pass had copied everything. On the test box a pass was running 22 % of the
  time and each hold kept writes ~200 ms. With the server's disk writes capped at 25 MB/s (it writes ~21 MB/s at
  300k) a pass took 0.45 s (up to 1.1 s), nearly every hold ran into its 250 ms timeout (186 in one run), the WAL
  passed twice its mark, and p95 / p99 went to 154-163 / 590-875 ms with no more CPU used: the tail of a slow disk.
  Now the WAL is left to grow and writes don't wait: same cap, p95 / p99 16 / 215 ms in a run that reached the WAL's
  bound three times (see below; 15 / 73 ms in one that didn't), 15 / 28 ms without the cap. A reader loses nothing by a
  long WAL: it looks up only the frames not copied yet (SQLite starts a read at the first frame that isn't in the
  database), and the passes keep those to about one pass's worth. The passes copy and the WAL is synced as before;
  what is put off is only the rewind. A longer pass also writes less: the writes land on about 13,000 index pages,
  and a pass writes each page once however many of its frames hold it, so a pass over 16,000 frames writes fewer
  pages per frame than one over 4,000: the disk got 4-5 times fewer write requests under the cap (26,091-28,911 a run
  against 145,203-154,034).
  The WAL has a bound: 4 GB, or half of the free space on the database's disk at startup if that is less (the startup
  line says which). Near it the passes follow one another, each a little shorter, and the checkpoint thread also
  syncs the database file after each (while writes flow); when a pass leaves at most 2,000 frames, writes wait for one
  more, short pass (up to 1.5 s, `HOLDRIDE`; SQLite's own sync at its end has only that pass's pages left), and the
  first write after it rewinds the WAL, alone in its commit. At 300k users the WAL grows about 16 MB a second, so
  with 4 GB that is once in four minutes; under the 25 MB/s cap such a hold kept writes 0.2 to 1.4 s. A WAL
  left by an earlier run is copied into the database and emptied at startup (`wal_checkpoint(TRUNCATE)`), so every
  run starts at zero. After each pass the thread drops the WAL's copied frames from the kernel's cache (no reader
  goes back to them), so the file's length doesn't cost memory. The database file's pages go too, after the pass
  that syncs the file (the held one).
- **SQLite.** Opened through the `unix-excl` VFS: the process holds the file's lock for its life and the WAL index
  lives in its memory, shared by its two connections. 64 MB page cache, no mmap, statements prepared once. Startup
  reads the indexes that writes touch into the page cache. The pages the load keeps coming back to are about 40 MB
  of the 250 MB seed (the two indexes that writes land on at random, 35 MB, the users table, and the newest end of
  everything else: a post is read and liked within seconds of being listed), and they grow by a few MB a minute. A
  cache with room for everything (256 MB before) also keeps every page ever appended, about 0.26 MB a second at 300k
  users, in anonymous memory that reclaim can't take back when the box is at its memory wall; at 64 MB the oldest of
  those give way instead. Inserts bind `created_at` in the column default's format
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
- **The process's own pages are anonymous memory.** The box has no swap, so near the memory wall reclaim can only
  take file-backed pages, and the pages of a mapped library are file-backed like any other: the loop thread then
  stalls on major faults that read its own code back from disk. An entry that is one small binary locks it in RAM;
  the .NET runtime maps 66 MB of images (about 30 MB of them in use) and `mlock` is capped at the 8 MB
  `RLIMIT_MEMLOCK`. Under memory pressure on the test box the loop thread took 6,000 to 92,000 major faults in a
  two-minute hold and read 0.8 to 12 GB of its own code and data back from disk; with all 66 MB locked at startup
  (a raised limit, to test) it still took 13,000, nearly all on two pages of assemblies that load at the first write.
  So `Pages.fs` turns every page of those files that is in use into an anonymous page with the same content, in
  place, at startup and then by the loop once a second (every 8 s once nothing new turns up) for pages first used
  later: a private mapping's page is copied on write
  (`MADV_POPULATE_WRITE` for a writable one; a read-only or executable one by writing its own bytes back through
  `/proc/self/mem`, which the kernel allows on a private mapping without a change of protection, the way a debugger
  plants a breakpoint), and a run of pages of a read-only shared mapping (the runtime maps assemblies whole that
  way) is replaced by an anonymous copy with one `mremap`. An anonymous page can't be reclaimed without swap. The
  kernel's cached copy of a moved page, which nothing maps any more, is dropped at once (`POSIX_FADV_DONTNEED`) when
  the file is one of the entry's own (the ones in its `out/` folder); the cache of the system's files (the runtime,
  libc) is left alone, and the kernel takes those copies back itself when memory gets tight. The same test
  afterwards: 0 major faults on the loop thread. The 8 MB lock stays, for pages not used yet. `ANON=0` turns it off.

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
- **Post:** `GET /posts/:id` reads post X and its author through cursor pair X mod 32 (`POSTSLOTS`, 0 for one
  shared pair). The posts asked for are the few that feeds list, so a pair mostly stays on one of them and the move
  returns at once instead of descending from the root; the row and its like count are still read for every request.
  Test box, 300,000 requests for the 20 newest posts at random on 3,000 connections: 4,780 user-space instructions per
  request against 6,700 with one shared pair (2,620 cycles against 3,400).
- **Live reads:** every request reads its rows while it is served (rule 5). Only prepared statements, cursors, key
  descriptions and the four root pages are kept, and the schema cookie is checked once per transaction. A read transaction
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
| `src/Pages.fs` | the process's own file-backed pages in use, moved to anonymous memory |
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
`LISTENERS` (8), `ANSWERS` (64), `WWIN` (ms, 20), `CKPASS` (frames, 16000), `HOLDMAX` (frames, 1048576), `HOLDRIDE` (ms, 1500), `BUFS` (1024), `CACHE_KB` (64000), `BTREE` (1), `VFS`
(`unix-excl`), `WALBUF` (1; 0: WAL frames written one by one), `PCACHE` (1; 0: pcache1 as it is), `ANON` (1; 0: the
process's file-backed pages stay file-backed), `STATS` (1: a line a second on stderr).

## Credits

The loop's policies (accept high-water marks, bounded re-arms, answers per pass, newest-first under backlog, the write
window, checkpoint holds) come from the author's [objc-uring-delneg](../objc-uring-delneg), and `Ring.fs` and `Jwt.fs`
from the author's F# entries. Ideas from other entries: the `unix-excl` VFS and eight `SO_REUSEPORT` listeners from
[rust-iouring-kensac](../rust-iouring-kensac), and calling SQLite's `sqlite3Btree*` functions on the unmodified
amalgamation built with `SQLITE_PRIVATE` defined empty from [swift-iouring-akbashev](../swift-iouring-akbashev),
and writing each commit's WAL frames with one `pwrite` at the commit frame, and the page cache's truncate walk, from
[c-iouring-pavlyhalim](../c-iouring-pavlyhalim), and that reclaim takes a server's own code at the memory wall (it
locks its binary in RAM and counted the loop's major faults) from [mojo-iouring-oh-meh](../mojo-iouring-oh-meh).

MIT license (the repository's).
