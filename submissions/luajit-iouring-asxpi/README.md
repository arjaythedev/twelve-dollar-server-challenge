# LuaJIT, no framework: io_uring through the FFI + SQLite B-tree reads

The server is Lua. One thread runs an io_uring event loop driven straight from LuaJIT's FFI (no
liburing), an HTTP/1.1 parser, a JSON reader and writer, HS256 verification and the handlers. The
only C is `shim.c`, a thin layer over the unmodified SQLite amalgamation; a second thread there
runs WAL checkpoints.

| | |
|---|---|
| Language | Lua on LuaJIT 2.1, built from a pinned commit (`63c8f1a`, sha256-checked by `install.sh`) |
| Framework | none |
| SQLite | 3.54.1 amalgamation, sha256-checked, fetched and compiled by `build.sh` with `-O3 -march=native` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings). There is no epoll fallback. A buffer-ring registration that Ubuntu's 6.8.0-136+ kernels refuse ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)) is retried once with `resv[0]=1` |

## Credits

The design follows [rust-iouring-kensac](../rust-iouring-kensac) (#264): direct descriptors over
several rings, eight `SO_REUSEPORT` listeners, drain-first passes, B-tree reads through a C shim,
one transaction per pass, checkpoints on a second thread with a hold on writes. `shim.c` follows
that entry's `csrc/shim.c`: the same structure, and several of its functions. Everything in
`src/` is Lua written for this entry.

Ideas from other entries: a pair of cursors per feed post from
[c-iouring-PROGABBOMALA](../c-iouring-PROGABBOMALA) (#430); one write per commit's WAL frames
through a VFS from [fsharp-uring-delneg](../fsharp-uring-delneg) (#398), whose README describes
the recipe used here; a page-cache wrapper that skips empty truncates, the design of
[lean-c-iouring-loafecho](../lean-c-iouring-loafecho) (#239); backlogs answered newest first with
an age cap, from [objc-uring-delneg](../objc-uring-delneg) (#326) and
[c-iouring-unorig-username](../c-iouring-unorig-username) (#362); writes committed in a window,
from fsharp-uring-delneg (#398) and objc-uring-delneg (#326); a read transaction kept across
read-only passes, from rust-iouring-rkkautsar (#355).

## Reads go through SQLite's B-tree, not its SQL engine

`GET /feed` and `GET /posts/:id` use SQLite's own B-tree cursor functions inside the pass's
transaction. `shim.c` `#include`s the unmodified amalgamation and only moves cursors through
`sqlite3Btree*` functions; it writes no cursor or B-tree state, only the search keys it passes
to those functions (a `KeyInfo` per index and the `UnpackedRecord` for the likes seek). Lua
decodes the record bytes and writes the JSON. Writes go through SQL (`INSERT … RETURNING`, `INSERT … ON CONFLICT DO
NOTHING`).

- **Feed.** `posts_created_at_id_idx` is `(created_at DESC, id DESC)`, so its first 20 entries are
  the feed. When the 20 ids fall within 256 of each other, the like counts come from one range
  walk of `likes_post_id_idx`; the post rows and author rows are then read per post.
- **A cursor pair per feed post.** Each feed post has a posts cursor and a users cursor of its own
  (post id modulo 20). Within a transaction the feed is mostly the same from one request to the
  next, so the next feed request asks those cursors for the rows they are already on and
  `sqlite3BtreeTableMoveto` returns at once instead of searching from the root. **What is kept
  between requests is a cursor's position, and only until the transaction's COMMIT closes the
  cursors (after one pass, or up to 50 read-only passes, below); the row bytes are read from
  SQLite's page for every request** (rule 5). An insert into `posts`
  makes SQLite save those cursors' positions, and their next use searches again. This is the
  technique of #430.
- **Pages.** SQLite's own page cache (64 MB) holds the pages; the file is not mapped by default
  (`TD_MMAP_MB` maps it: SQLite then consults the WAL index on every page access, which cost
  about 1 µs per request here). The page cache is pcache1 behind a wrapper
  (`SQLITE_CONFIG_PCACHE2`, a public interface): B-tree balancing parks pages at the lock-byte
  page's number, after which pcache1 scanned its whole hash table at every COMMIT to drop
  nothing; the wrapper tracks which keys may be cached and skips such truncates.
- **No assumptions about the data.** Anything unexpected (ids not descending, a wide id range, a
  missing post or author row) hands the request to a plain plan with one seek per row, and that
  plan is what answers when a post's author is missing. At startup the server checks the column
  order of every table and index it decodes and refuses to start on a different schema.
  `TD_FEED_PLAIN=1` forces the plain plan; the test suite passes both ways, and both plans gave
  identical responses over 2,167 scripted requests mixing feeds, new posts and likes.

## Durability and batching

- WAL, `synchronous=NORMAL`, opened through SQLite's `unix-excl` VFS: the process holds the file
  for its life and the WAL index lives in its memory, shared by its two connections.
- **One transaction per pass, reads first.** The loop waits for up to 1 ms or 256 completions,
  then works in two phases inside one transaction. Read phase: every read that arrived is
  answered; writes wait in their connections' input. No write has run yet, so the reads see only
  committed data, and their responses go to the kernel at once. Write phase: the writes run,
  group-committed, at most every 10 ms (fewer commits mean fewer WAL frames per write, so fewer
  checkpoint rounds). **No write's response is handed to the kernel before its COMMIT returns**; if
  the COMMIT fails, the write responses become 500s and their connections are closed. A read
  pipelined behind a write on the same connection waits until the write's response has been sent,
  so responses stay in order and **no read ever sees an uncommitted write**. `TD_READS_FIRST=0`
  answers reads and writes in arrival order and sends everything after the COMMIT.
- **A read-only pass keeps its transaction.** A pass that wrote nothing does not COMMIT; the next
  pass reads in the same transaction, with its cursors still open. The snapshot is still the
  latest committed state, because this loop is SQLite's only writer and every pass that writes
  commits. **Every request still reads its rows from SQLite's pages while it is served; what is
  kept is the transaction and the cursors' positions, not data.** The transaction ends after 50
  passes, at the first write, and when a checkpoint has copied the whole log (so the next write
  can restart it). `TD_KEEP_READ=0` commits every pass. The approach of rust-iouring-rkkautsar
  (#355) and fsharp-uring-delneg (#398).
- **One write per commit.** SQLite writes each WAL frame as two writes. A small VFS over
  `unix-excl` (SQLite's public VFS interface) collects the WAL writes of a COMMIT and writes them
  when the commit frame's page arrives, still inside SQLite's own write call, so the commit is in
  the kernel before SQLite publishes it in the WAL index and before COMMIT returns. Reads, syncs,
  truncates, size queries and closes write the buffer out first. `TD_WALBUF=0` turns it off.
  Killed with `kill -9` mid-load six times with checkpoints every 256 frames, all 18,288 posts and
  8,988 likes acknowledged before the kill were in the database and `integrity_check` was ok.
- **Checkpoints** run on a second thread and connection, so the loop never waits for their
  syncs. After 4,096 frames the thread runs a PASSIVE round; writes are then held (reads go on)
  while a short second round copies what arrived meanwhile, and the next write restarts the log.
  The thread has the loop's priority and sleeps between rounds (at idle priority between
  rounds, its rounds waited hundreds of milliseconds behind a busy loop). A hold that
  ends unfinished doubles the next one's limit (250 ms up to 2 s), and past 65,536 frames writes
  stay held until the log is copied. Without these the log stopped restarting under full load
  and grew until the server ran out of memory. After a round that did not finish, the thread
  syncs the database file itself so the sync inside the hold is short.

## Connections

- Accepted sockets are io_uring **direct descriptors**, which do not count against
  `LimitNOFILE`. A ring's table holds 65,535; six rings are made at startup (creating one takes
  0.2 s, which stalled the loop when done mid-ramp) and more are added on demand. A ring stops
  accepting 5,000 slots before it is full and another takes over.
- Eight `SO_REUSEPORT` listeners with multishot accept, so 32k connections can wait in the
  listen queues during a ramp.
- One **multishot receive** per connection into a provided-buffer ring: an idle connection holds
  no buffer. Each ring has 16,384 buffers of 1 KB, enough for a burst of tens of thousands of
  requests after a pause of the clients (with 4,096 the rings ran dry and receives stopped).
  Receives that end with `ENOBUFS` anyway are re-armed a bounded number per pass.
- **Drain first.** A pass reads up to 16,384 completions but answers at most 2,048 requests; the
  rest are copied to their connections, in 1 KB buffers from a pool, and answered by later passes.
  Shorter passes (a cap of 256, or a 10 ms time limit, `TD_PASS_US`) cost too much capacity: each
  pass has fixed costs, and in a crush the server fell behind. A request left in its socket holds
  a 3–4 KB kernel buffer.
- **Backlogs newest first.** A pass answers new arrivals first, then queued requests: those
  queued longer than 8 s first, the rest newest first. (While new arrivals alone fill every pass,
  the queue waits; the age cap applies once the pass has room.) Oldest first, every request in a
  backlog waits for the whole queue; newest first, most are answered quickly and a few wait
  longer, the age cap bounding that wait. A reply within 40 ms also spares the kernel a separate
  ACK. Nothing is dropped, and requests of one connection are answered in order. The approach of
  objc-uring-delneg (#326) and c-iouring-unorig-username (#362). `TD_LIFO=0` answers oldest
  first.
- Idle connections are never closed by the server.

## Request path

- **HTTP:** `memchr` finds the end of the head and of each line; only `Content-Length`,
  `Authorization`, `Connection` and `Transfer-Encoding` are looked at.
- **JWT:** the key's two padded blocks are hashed once at startup. Every token's signature, `alg`
  (HS256 only) and `exp` are checked on every request, the signature in constant time. SHA-256
  is plain Lua, with eight rounds written out per loop iteration.
- **JSON out:** almost no string needs escaping, so `td_json_clean` in the shim finds the length
  of the prefix that can be copied as it is (a byte scan, eight bytes at a time) and Lua copies it
  in one piece. This is the one piece of JSON work in C. It was Lua at first, as generated
  straight-line code; inlined into the feed loop its branches became side traces LuaJIT could not
  assemble, and the same build ran at 22 or at 60–100 µs per request depending on how the traces
  happened to form. A call into C is a fixed boundary.
- Nothing on the request path allocates: requests, records and responses live in fixed buffers.

## Files

| | |
|---|---|
| `src/main.lua` | rings, accept, receive, send, close, the pass loop, the checkpoint policy |
| `src/app.lua` | HTTP, routes, JWT checks, record decoding, the feed and post plans, responses |
| `src/json.lua` | JSON reader and writer |
| `src/sha256.lua` | SHA-256 and HMAC |
| `src/cdefs.lua` | every C declaration used: libc, the io_uring ABI, the shim |
| `shim.c` | the amalgamation plus B-tree cursor and statement helpers, the WAL VFS, the checkpoint thread |

SQLite options (in `build.sh`): `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`,
`DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `USE_ALLOCA`, `OMIT_DEPRECATED`,
`OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`,
`OMIT_TRACE`, `OMIT_COMPLETE`, `OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

Optional settings, all with defaults: `TD_WAIT_US` (1000) and `TD_BATCH` (256), the pass window;
`TD_RINGS` (6); `TD_NBUF` (16384, receive buffers of 1 KB per ring); `TD_ANSWER_CAP` (2048) and
`TD_PASS_US` (0, no limit), when a pass stops answering; `TD_READS_FIRST` (1); `TD_WRITE_US` (10000);
`TD_LIFO` (1) and `TD_LIFO_AGE_MS` (8000); `TD_KEEP_READ` (50); `TD_CKPT_FRAMES` (4096),
`TD_HOLD_MS` (250), `TD_HOLD_MAX_MS` (2000), `TD_CKPT_FORCE` (65536); `TD_CACHE_KB` (65536);
`TD_MMAP_MB` (0); `TD_PCACHE` (1); `TD_WALBUF` (1); `TD_FEED_PLAIN` (0);
`TD_STATS=1` prints one line of counters a second on stderr.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
