# Goose: io_uring, a write queue, and SQLite's B-tree cursors

A port of the ideas in [rust-iouring-kensac](../rust-iouring-kensac) (#264) to [Goose](https://github.com/aardappel/goose), to see whether the same design gets any faster in a language with no heap, no GC and no lifetime annotations, which compiles to C. One thread serves every connection through io_uring and every request through one SQLite transaction per pass. The code is new; the design is kensac's, with the changes listed under [What differs](#what-differs-from-the-rust-entry).

| | |
|---|---|
| Language | Goose, compiler pinned at commit `369c7a2c` (runtime `3025e5810379`), built from source by `install.sh` (sha256-checked) |
| Framework | none: the event loop, HTTP/1.1, JSON and HS256 (SHA-256 and HMAC included) are written in Goose here |
| io_uring | raw system calls against the kernel's `<linux/io_uring.h>`; no liburing |
| SQLite | 3.54.1 amalgamation (sha256-checked), unmodified, compiled into `db.c`; opened through a small VFS of its own over `unix-excl` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, link-time and profile-guided optimization, static |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors). No epoll fallback, so it does not start where io_uring is blocked (Docker's default seccomp profile) |

## What is Goose and what is C

| `server.goose` (1,800 lines) | `net.c`, `db.c` (1,100 lines) |
|---|---|
| the loop: waiting, draining every ring's completions, flushing after COMMIT, re-arming receives | `io_uring_setup`/`enter`/`register`, queueing operations, copying completions into a Goose array |
| the connection table: generations, flags, what a connection is waiting for | the bytes the kernel reads or writes after a call returns: receive buffers, pending output, the send in flight, input kept for later |
| accepting: how many in flight, on which ring, when a ring is full, when to add one | rings, sparse direct-descriptor tables, listening sockets |
| the write queue, group commit, the checkpoint policy | the checkpoint thread (it copies when asked) |
| HTTP/1.1 framing and parsing, routing, every status code and error message | |
| HS256: base64url, SHA-256, HMAC (pad states precomputed), the claims | |
| a validating JSON reader (any document, escapes, surrogate pairs, UTF-8) and the JSON writer | |
| JavaScript's `trim()` on the body and its code-point count | |
| SQLite's record format: varints, serial types, column ranges | moving SQLite's B-tree cursors, copying a row's record bytes |
| the feed and post read plans, and their fallback to SQL | BEGIN/COMMIT, the two INSERTs, `SELECT 1` |

A Goose `extern fn` may not keep a borrowed argument after the call returns, so no Goose memory is ever shared with the kernel: C appends received bytes and record bytes to Goose builders, and a finished response is copied into a C chunk for the send. Neither C file decides anything.

## A pass

1. Every other ring submits and collects without waiting, and a ring with nothing queued and no deferred work pending (the kernel says so in the ring's flags) costs no system call. Ring 0 goes last and waits for 256 completions or 1 ms, so a busy pass shares one transaction across many requests.
2. Each ring's completions are copied into a Goose array and handled in order. A receive is served at once: every complete request in the message is parsed and answered into the connection's pending output; an incomplete one is kept with the connection.
3. Reads run right away, inside the pass's SQLite transaction (begun by the first request that needs the database). Writes are validated and queued (below).
4. If the write queue is due, it runs.
5. `COMMIT`, and only then does every touched connection's output go to the kernel: **no response of a pass is sent before its COMMIT returns**. If the commit fails, every answer of the pass becomes a 500 and its connection closes, so a 201 never claims a write that was lost, and no client sees a row that was not committed.
6. Receives that ended are armed again, accepts are topped up, and the checkpoint policy runs.

## The write queue

A `POST` is checked when it arrives: the token's signature, `alg`, `exp` and claims, the id, the JSON body, the trim and the length. A request that fails is answered in that pass. One that passes becomes a record in a Goose array of variable-size records (`Job[>..]`: the connection, the user id, the post id or the username and trimmed body, stored inline one after another), and its connection reads no further request until that write is answered, so responses keep their order.

The queue runs when the window that opened with its first write ends, 5 ms later: all queued writes run in arrival order in one transaction and share its COMMIT (group commit, rule 6: each response waits for the commit of its own write). The pages several writes touch (the tail of `posts`, of `likes` and of their indexes) then reach the log once instead of once per pass. Measured here at 2,700 writes/s: 1.4 log frames per write with the 5 ms window, 1.65 with a 1 ms one.

The 404 check of a like (does the post exist) and both INSERTs happen when the queue runs, in that transaction. Nothing about the benchmark is detected: every write takes this path.

## Reads

`GET /feed` and `GET /posts/:id` move SQLite's own B-tree cursors inside the pass's transaction, so only the SQL bytecode engine is skipped: pages come through SQLite's pager, WAL and page cache (as the organizer ruled on 2026-10-31). `db.c` moves a cursor and copies the row's record bytes into a Goose builder; Goose decodes the record and decides what to read next.

- **Feed:** the first 20 entries of `posts_created_at_id_idx`, which is `(created_at DESC, id DESC)` and so exactly the reference `ORDER BY`. Then the 20 posts by rowid in ascending order (neighbours on a leaf are one step apart), their authors in `users`, and the like counts: one walk of `likes_post_id_idx` from the lowest id to the highest when the ids span at most 512, a seek per post otherwise.
- **Post:** a rowid seek on `posts`, one on `users`, and the like count.
- **Nothing about the data is assumed**: not dense ids, not timestamps in id order. A post whose user row is missing (which the JOIN would drop) sends the feed to the reference SQL query. The column order of every table and index that is decoded is checked at startup, and the server refuses another schema.
- **Only SQLite's B-tree interface is called** (`sqlite3BtreeTableMoveto`, `IndexMoveto`, `First`, `Next`, `PayloadSize`, `PayloadFetch`, `Payload`), and **no SQLite structure is written from outside**. Like counts step the cursor entry by entry with `sqlite3BtreeNext` instead of setting the cursor's position on a leaf.
- Every request reads its rows from SQLite while it is served (rule 5). What is kept between requests: prepared statements, cursor objects, `KeyInfo`, the four root page numbers (schema metadata read at open), and the HMAC key schedule.

Writes are SQL: `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`, where `sqlite3_changes` tells a new like from a repeat.

## Durability and checkpoints

- WAL with `synchronous=NORMAL`, opened through SQLite's `unix-excl` VFS: the process locks the file once and keeps it, and the WAL index is heap memory shared by its two connections (no `-shm` file, no file locks per transaction). 64 MB page cache, no mmap, `wal_autocheckpoint=0`.
- A second thread on its own connection, at nice 19, runs one PASSIVE checkpoint each time `server.goose` asks, so the loop never waits for a checkpoint's fsyncs. The policy is in Goose: past 16,384 log frames (64 MB) ask for a copy; when it finishes, hold the write queue (reads go on) and ask again, which copies the frames written meanwhile; when the whole log is copied the next write starts the log over. A hold lasts at most 200 ms. Past 40,960 frames the queue is held as well, so the log stays bounded if the loop leaves the thread no idle time.
- **One write per commit.** SQLite appends each log frame with two writes (a 24-byte header, then the page), so a commit of forty frames is eighty system calls. The connections open through "feed", a VFS over unix-excl made with SQLite's public VFS interface (the idea is goose-iouring-ahmadsh2007's). On the log file it gathers the headers and pages of frames appended at the log's end and writes them all the moment the page of a commit frame arrives: inside SQLite's own write call, so before the frames enter the WAL index and before COMMIT returns, the point by which SQLite would have written them itself. Only appends are held back: frames SQLite rewrites in place (a page written twice in a transaction that spilled, then the checksums after it) go straight through, and every read, sync, truncate, size query, file control and close first writes out what is gathered. The database file never passes through it. Under the load test's mix this takes file writes from 0.25 to 0.03 per request.
- `kill -9` tests, each on one database across all its rounds, the server killed at a random moment: with 16 writers and a checkpoint every 1,500 frames, six kills, 14,277 acknowledged posts and 14,489 acknowledged likes; with 64 writers and a 10-page cache, so that every transaction spills and rewrites frames, eight kills, 20,900 posts and 20,684 likes. Every acknowledged row was in the database afterwards and `integrity_check` said ok each time.
- A model test of the VFS alone: 3,000 random transactions (inserts, updates, deletes, rollbacks, page caches of 3 to 42 pages, checkpoints from a second connection), with the database and its log copied after a commit 740 times, as a crash at that instant would leave them. Every copy held exactly what had been committed. Both tests catch a VFS that also holds back the headers SQLite rewrites (the first version here did, and lost commits of transactions that spilled).

## Connections

- Accepted sockets are io_uring **direct descriptors** in a ring's file table, so `LimitNOFILE=65535` caps a ring, not the server. A ring takes accepts until fewer slots are free than the accepts in flight plus 64; then a ring with room takes over, and one is added when none has room (up to 16 rings: over a million connections). Accepts are single-shot, 512 in flight, spread over eight `SO_REUSEPORT` listeners, each with its own accept queue.
- One **multishot receive** per connection into the ring's provided buffers (16,384 × 1 KB), so an idle connection holds no buffer, only a 12-byte record in Goose and a 32-byte one in C. A receive that ends for lack of a buffer is armed again at the end of the pass, when the buffers the pass used are back. A 4 s stall under 33,000 requests/s ended 30,751 receives that way; the server caught up within a second, with no error.
- One send in flight per connection; answers to pipelined requests queue behind it in order. Closing is a `SHUTDOWN` hard-linked to a direct `CLOSE`. A generation number in every `user_data` drops completions for a slot that was reused. Idle connections are never closed.
- Ubuntu's 6.8.0-136+ kernels refuse a buffer-ring registration whose reserved words are zero (Launchpad #2162843); a refused registration is tried once more with `resv[0] = 1`.
- The program's code (static, about 1.5 MB) is `mlock`ed at startup: with no swap, reclaim under memory pressure takes file-backed pages first.

## HTTP, JSON, JWT

- Only the headers the API uses are read. Requests split across receives are completed from the bytes kept with the connection; pipelined requests are answered in order. Heads over 16 KB, bodies over 1 MB, chunked bodies and malformed request lines get `400 {"error":"bad request"}` and the connection closes (out of scope in SPEC). `Connection: close` and HTTP/1.0 close after the response.
- HS256 only. Every token's signature is checked on every request, in constant time; only the canonical base64url spelling of a signature is accepted. `exp` is required and must be in the future; then `sub` must be a positive integer string and `username` a string.
- The JSON reader validates whole documents with an explicit stack (no input can recurse), decodes escaped member names, and lets the last duplicate key win, as `JSON.parse` does. `body` is trimmed with JavaScript's `trim()` whitespace set and counted in code points, the unit of the schema's `CHECK (length(body) …)`.
- Output strings are checked for bytes that need escaping 32 bytes at a time (a loop of fixed length that gcc turns into vector instructions) or 8 at a time for short ones, and copied in one piece when none do. A post's body and `created_at` lie side by side in its record, so one check covers both.

## What differs from the Rust entry

- **The language.** Everything that decides is Goose. Where Rust holds a slice of SQLite's page or of the receive buffer, Goose gets a copy (the FFI rule above), so this entry copies more and still executes fewer instructions per request (below).
- **Writes wait in a queue** and commit together every 5 ms; kensac commits each pass (about every millisecond).
- **Larger receive-buffer rings** (16,384 buffers a ring, the count [goose-iouring-ahmadsh2007](../goose-iouring-ahmadsh2007) measured for #291) and re-arming at the end of the pass, instead of 1,024 buffers and bounded single-shot reads after a burst.
- **Like counts step the cursor** with `sqlite3BtreeNext`, entry by entry, so no field of a cursor is set from outside (ruling 8). Counting a leaf at a time, as the Rust entry does, costs less per entry; under the load test there is almost nothing to count: at 310,000 users it writes about 270 posts a second (2 % of 29,284 requests/s ÷ 2.17 requests per loop), so the feed's 20 posts are under a tenth of a second old and not liked yet.
- **One write per commit** through a VFS (above); kensac writes each frame as SQLite does.
- **Profile-guided and link-time optimized**, SQLite included (kensac builds SQLite at `-O3`).
- No crates and no liburing: the ring is set up through system calls.

## Build

`install.sh` (root, once): `build-essential cmake ninja-build curl unzip python3`, then the Goose compiler from its pinned source, with one job, `-O2` and a tighter gcc collector so that it fits in the box's 2 GB (the settings of [goose-native-tbsvttr](../goose-native-tbsvttr) #143).

`build.sh` (user): fetches and checks SQLite, compiles `server.goose` to C (`goose -O2 --standalone`), and builds `bin/server` static at `-O3 -flto` (`-march=x86-64-v3 -mtune=cascadelake`, 256-bit vectors, Ubuntu's default hardening off). **Profile-guided**: an instrumented build first serves `train/train.py`, which makes a synthetic database of its own (30,000 users, 150,000 posts, 600,000 likes; never the seed), signs its own tokens, and drives the load test's request mix plus the test suite's error paths. `PGO=0 bash build.sh` skips it. The SIGTERM-to-`exit()` hook that writes the profile (`train/pgo_exit.c`) is linked into the instrumented build only.

SQLite options: `THREADSAFE=2` (each connection used by one thread), `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `USE_ALLOCA`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`, `OMIT_COMPLETE`, `OMIT_GET_TABLE`, `OMIT_TCL_VARIABLE`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

## Rules

1–3. The SQLite file at `$SQLITE_PATH`, in-process; schema untouched; reads through SQLite's B-tree cursors and pager, writes in SQL.
4. `test/test.sh`: 42/42.
5. No caching across requests: every request reads its rows from SQLite while it is served. Scratch buffers are reused as storage only.
6. WAL + `synchronous=NORMAL`; group commit; each response waits for the commit of its pass.
7. Nothing detects the load test.
8–9. Direct on `HOST:PORT`.
10. Built from source on the box; Goose and SQLite pinned by commit/version and sha256.
11. Nothing on the box is tuned; the process raises its own soft `RLIMIT_NOFILE` to the hard limit.

## Local measurements (not a score)

MEASUREMENTS

Behaviour, request for request against rust-iouring-kensac on fresh seed copies: 214 raw-request cases (routing, every token failure, 80 JSON bodies with escapes, surrogate pairs, Unicode trimming, multi-byte 500-character limits, duplicate keys, nesting and invalid UTF-8, pipelining, requests split byte by byte, half-closed connections, malformed framing) give the same status and body in all but four, by intent: this entry closes after `Connection: close` and HTTP/1.0 (three cases), and answers 400 to a 17 KB request head.

Test-only knobs, never set on the box: `FEED_SLOTS` (a ring's table size; 2,000-slot tables served 10,000 users across six rings with no errors), `FEED_WRITE_US`, `FEED_CK_FRAMES`, `FEED_WAIT_US`, `FEED_PLAIN_LOG` (plain unix-excl, without the VFS), `FEED_CACHE_PAGES`, and `FEED_STATS` (a line a second: requests, passes, completions, writes per write pass, the longest pass split into serving, writes and commit, receives ended and re-armed, the log, holds and restarts).

## Credit

The design is rust-iouring-kensac's (#264, MIT): direct descriptors in several rings, single-shot accepts kept in flight over `SO_REUSEPORT` listeners, multishot receives into provided buffers, the buffer-ring retry for Ubuntu's kernels, one transaction per pass with responses held until COMMIT, B-tree cursor reads with a SQL fallback and a schema check, and the checkpoint thread with writes held while the log's tail is copied. From goose-iouring-ahmadsh2007 (#291, MIT), which did this port first: the receive-buffer count, the idea of letting writes wait a few milliseconds so that they share a commit (there the request waits unparsed; here it is validated first and queued as a record), and the idea of a VFS that writes a commit's frames at once. Locking the code in memory, the hardening-off flags and PGO on a synthetic database come from c-iouring-pavlyhalim (#245) and mojo-iouring-oh-meh (#257); reading through SQLite's B-tree cursors from lean-derive-hargup (#153). All code here was written for this entry.

## Files

| | |
|---|---|
| `server.goose` | the server |
| `net.c` | the kernel: rings, queued operations, completions, receive buffers, per-connection byte chunks, listeners |
| `db.c` | SQLite: the amalgamation, cursor moves and row copies, the pass's transaction, the INSERTs, the checkpoint thread, the log VFS |
| `ffi.h` | the two Goose shapes that cross into C |
| `train/train.py`, `train/pgo_exit.c` | profile training (build time only) |
| `install.sh`, `build.sh`, `start.sh` | |
