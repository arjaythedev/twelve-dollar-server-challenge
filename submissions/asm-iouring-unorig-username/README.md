# x86-64 assembly (NASM) + io_uring + SQLite B-tree reads

A port of our C entry, [c-iouring-unorig-username](../c-iouring-unorig-username) (PR #362 at `cae940e`, plus two
later changes described below), to hand-written x86-64 assembly. The io_uring event loop (raw
`io_uring_setup`/`enter`/`register` syscalls on hand-mapped rings, no liburing), HTTP/1.1 framing, routing,
a strict JSON validator and decoder, JSON output, base64url, SHA-256/HMAC-SHA256, the JWT checks, the decoding of
SQLite's B-tree records, the feed plan, the likes walk, group commit and the checkpoint policy are in
`server.asm`.

**The SQLite access is split, the same way as [asm-btree-ninhnv](../asm-btree-ninhnv) (#388):** the assembly
reaches SQLite's B-tree layer through a small C shim, `shim.c`, which `#include`s the unmodified amalgamation
because the B-tree functions and structures are `static` inside it and have no public API. The shim opens
the database, checks the schema, starts and ends the B-tree read transaction and its four cursors, moves
cursors, returns the record bytes under a cursor and a view of the likes cursor's page, and runs the
checkpoint thread and the WAL write coalescing (SQLite calls those through its VFS as C functions). It does
not decode records or build responses. Every SQL statement (the two INSERTs, `BEGIN`/`COMMIT`/`ROLLBACK`,
`SELECT 1` and the reference queries used as a fallback) is prepared and stepped from the assembly through
SQLite's public `sqlite3_*` API.

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 `nasm`), System V ABI throughout: `server.asm`, 6,087 lines |
| C | `shim.c` only, 253 lines: database glue around the amalgamation's internal B-tree calls (above), ported from the database section of our C entry's `server.c` |
| SQLite | 3.54.1, the official amalgamation, unmodified, SHA-256 pinned in `build.sh`, compiled into the shim with `gcc -O3 -march=native` |
| Linking | `gcc -no-pie` against glibc (`main` is the entry point, so SQLite's C runtime is set up); the assembly calls `malloc`/`realloc`/`free`, `time`, `clock_gettime`, `strtod` and `dprintf` |
| Dependencies | none besides SQLite and libc: no HTTP, JSON, crypto or io_uring library |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

Needs Linux 6.1 or newer with io_uring enabled (Ubuntu 24.04's 6.8 kernel has it on by default).

## Build and run

```bash
sudo bash install.sh     # apt: build-essential, nasm, curl, ca-certificates, unzip
bash build.sh            # downloads and verifies SQLite, compiles the shim, assembles, links bin/server
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design (the C entry's, ported)

- **One event-loop thread.** A second thread (in the shim) only runs checkpoints. Every ring is
  `DEFER_TASKRUN` and `SINGLE_ISSUER`, so completion work runs when the loop enters a ring and never
  interrupts a pass. Each ring's own descriptor is registered (`IORING_REGISTER_RING_FDS`), so the enters skip
  the descriptor lookup. Each pass gives every extra ring one non-blocking `io_uring_enter`, then waits on ring 0.
- **Wake-ups are batched when busy.** If the last pass handled 4 or more completions, the wait asks for ring 0's
  share of 128 completions (capped at a third of open connections) or 2 ms, whichever is first; with more than
  one ring, or while POSTs are held, the wait is always timed. Responses still wait for their batch's commit.
- **Accepted sockets are registered (direct) descriptors**, allocated with `IORING_FILE_INDEX_ALLOC`. A table
  can't exceed `RLIMIT_NOFILE`, so the server raises its soft limit to the hard one and adds rings (up to 32)
  as tables fill.
- **Eight `SO_REUSEPORT` listeners, single-shot accepts, 64 in flight on each**, only while a table has room
  (the last 16 slots are never used), so an accept never lands on a full table.
- **Multishot receives into provided buffer rings** (2,048 x 1 KB per ring). The registration retries once with
  `resv[0] = 1` for the Ubuntu 6.8.0-136+ kernels that reject all-zero reserved words (Launchpad #2162843).
- **After a stall, reads and answers are bounded.** A receive that runs out of buffers is not rearmed: its
  connection goes on a FIFO read queue, read by single-shot `MSG_DONTWAIT` receives, never more on a ring than it
  has buffers, and back to a multishot receive once drained. A pass does at most 4,096 of these reads and
  answers 1,024 requests; the rest wait on their connection, on a FIFO ready list, answered after the requests
  that have just arrived.
- **One transaction per pass (group commit).** The first request that touches the database runs `BEGIN` and
  starts the B-tree read transaction; every read and write in the pass runs inside it, and the pass ends with one
  `COMMIT`. Responses are written into an arena and handed to `send` only after that `COMMIT` returns, so a
  `201` always means the row is committed. If a commit fails, nothing from a pass with writes is sent and those
  connections are closed.
- **Reads walk SQLite's B-tree cursors.** `/feed` walks `posts_created_at_id_idx` for the 20 newest, sorts their
  ids, counts their likes in one walk of `likes_post_id_idx` when they lie within 512 ids (the assembly tallies
  every cell of a leaf from the shim's page view and asks SQLite to move to the next leaf), fetches the rows in
  ascending id order, looks up each author, and puts the rows back in feed order. `/posts/:id` is the same plan
  for one row. The assembly decodes the record headers (varints, serial types) and writes JSON straight from
  the record bytes. A like's post check is a B-tree seek.
- **SQL is the fallback.** At startup the shim checks the columns and sort order of everything the assembly
  decodes; on any difference every read uses the reference SQL. At run time anything unexpected (a record on
  overflow pages, a post without a user, a column of another type, a missing post) sends that request to SQL.
  Writes always use prepared SQL.
- **Checkpoints: the shim's worker thread copies, the loop decides.** The worker runs at nice 19 on its own
  connection, through the `unix-excl` VFS (the WAL index lives in the heap, no file locks). The loop asks for a
  `PASSIVE` pass over an eventfd after 8,192 WAL frames; the outcome comes back on a second eventfd as a ring
  completion, so the loop never waits for an fsync. If frames arrived during a pass, POSTs stay unread for one
  more pass, for at most 250 ms (reads go on); past 32,768 frames they wait for that pass however long it takes.
  Every 8 MB of new log, the loop queues `SYNC_FILE_RANGE_WRITE` writeback of it on an io-wq thread, so dirty WAL
  pages never pile up to the kernel's dirty threshold.
- **Hot memory sits on huge pages**: the page cache (one contiguous `SQLITE_CONFIG_PAGECACHE` region, 32 MB), the
  receive buffers, the connection tables and the response arena are `MADV_HUGEPAGE`.
- **Sends** go one at a time per connection (later responses queue behind it), each from its own copy, from a
  LIFO pool of 8 KB buffers. Closing cancels the pending receive and send by their completion tags and closes the
  slot in one hard-linked chain; generation numbers in the tags make late completions harmless, and a closed
  connection's send buffer waits on a list until its cancel has been submitted.
- **SQLite settings**: WAL, `synchronous=NORMAL`, no `mmap`, `SQLITE_THREADSAFE=2`, prepared statements built once
  with `SQLITE_PREPARE_PERSISTENT`, unused features compiled out.
- **No caching** (rule 5): every request reads its rows from SQLite while it is served and every token's HMAC is
  computed; only the HMAC key schedule is precomputed. Kept between requests: prepared statements, cursor
  memory, two `KeyInfo` objects and four root page numbers.
- **Strict input handling**, as in the C entry: RFC 8259 JSON (last duplicate key wins), `\u` escapes and
  surrogate pairs decoded, invalid UTF-8 rejected, `body` trimmed with JavaScript's `trim()` set and counted in
  code points; HS256 only, constant-time signature compare, `exp` required and `exp`/`nbf` read with `strtod`.
  Keep-alive with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and `Expect: 100-continue`. Chunked
  request bodies aren't supported (out of scope); such a request closes the connection.
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard limit.

### The two changes on top of `cae940e`, on by default

`build.sh` passes `COMBO=1` to both the assembler and the shim; `COMBO=0 bash build.sh` builds the `cae940e`
behaviour. They are the C entry's best local candidate (`4298dd7`), not scored on the benchmark box:

- **One `pwrite` per commit's WAL frames** (shim). SQLite writes each frame as two `pwrite`s (header, page). Through
  the VFS's `xSetSystemCall`, the loop thread gathers a commit's frames and writes them in one `pwrite` once the
  commit frame is complete, before SQLite updates the log index; a read or `fstat` of the same file flushes first.
- **A send pool of 2,048 buffers** (assembly), at least one pass of answers, instead of 512.

### What differs from the C entry

- No profile-guided or link-time optimisation: the C entry trains a PGO profile; here only the shim and SQLite are
  compiled (`-O3 -march=native`), and the assembly runs as written.
- String escaping checks 16 bytes per step with SSE2 (`pminub`/`pcmpeqb`/`pmovmskb`) instead of the C entry's
  8-byte SWAR test. The assembly uses nothing beyond baseline x86-64 (SSE2).
- If adding a ring fails, no further rings are tried (the C entry retries on the next pass).
- Its diagnostics are fewer.

### Changes from the previous version of this entry

The previous version (250,000 on the board) ran SQL for every read, waited 6 ms per busy pass, ran checkpoints in a
forked helper process and had one listener. Porting the C entry's design replaced all of that. It also fixed a
bug: the close chain's cancel put the target tag in the SQE's `off` field instead of `addr`, so the kernel rejected
it (`EINVAL`), which also cancelled the hard-linked close. A closed connection's slot in the registered table was
never freed, and the server never closed its side of a connection the client had half-closed.

## Local verification

The benchmark box is x86-64; our machines are arm64, and Rosetta and qemu-user have no io_uring. So everything
below ran in an `ubuntu:24.04` amd64 container on a fully emulated x86-64 VM (Colima, QEMU, kernel 6.8.0-117),
with Docker's default seccomp profile off (it blocks io_uring).

- `test/run.sh` from a clean folder: **42/42**, with the default build and with `COMBO=0`.
- **Responses diffed against the C entry** (`4298dd7`, built in the same container) on fresh copies of the seed:
  the same request sequence sent to both, raw responses compared byte for byte (status line, headers, body),
  with only `uptime_s` (and that response's `Content-Length`) and the `created_at` of posts written during the run
  masked. **561 of 561 identical.** It covers `/feed`, 300 random `/posts/:id`, bad and missing ids, unknown routes
  and methods, 28 malformed or edge-case tokens on both write routes, 27 edge-case bodies (escapes, `\u0000`, lone
  surrogates, 500 and 501 code points, duplicate keys, invalid UTF-8), 52 likes (new, repeated, missing), reads of
  everything written, pipelining, `Connection: close`, HTTP/1.0, `Expect: 100-continue`, chunked and garbage
  requests, a request split across writes, and the server's EOF after each client half-closes.
- **The rare paths ran.** On a build with test-only overrides (`-DNBUF=16 -DANSWERS=4 -DWAL_START=16`) and a
  descriptor limit of 300, 400 clients sent 12,186 mixed requests (pipelined, split, reconnecting) under `gdb`
  with counting breakpoints: 4 rings added, 357 single-shot reads from the read queue, 6,338 ready-list visits,
  353 checkpoint passes that held POSTs, 344 deferred or held lists replayed, WAL writeback kicks, the B-tree feed
  and post plans. 0 errors, and all 1,194 posts that got a 201 read back with their bodies. The default build
  with a descriptor limit of 300: 500 clients, 20,266 requests, 4 rings, 0 errors, all 2,061 posts read back.
- **`kill -9` under write load**, 3 trials, 50 clients writing posts and likes for 6 s: 8,400, 9,600 and 9,053
  acknowledged posts and 8,372, 9,586 and 9,048 acknowledged likes, none missing, `integrity_check` ok.

### Not measured

- **Performance, at all.** Under full emulation throughput and CPU numbers mean nothing, so none are given. This
  port has never run on real x86 hardware, and not on the benchmark box.
- The `-march=native` build of the shim and SQLite on a real CPU (the emulated CPU exposes a different feature
  set). The assembly itself uses baseline x86-64 only.
- Behaviour near memory exhaustion, at hundreds of thousands of connections, or with a disk that fills up.
- A send completing for a connection that was already closed (the limbo list) never happened in these runs.

## Credits

- Our C entry, c-iouring-unorig-username (#362), whose design and database code this ports. Through it,
  rust-iouring-kensac (#248, #264): the shim's `key_info`, `rootpage`, `layout_is`, cursor seek and the setup of
  the likes index seek follow its `csrc/shim.c`, and the B-tree reads, `unix-excl`, the checkpoint thread with
  write holds, `SO_REUSEPORT` listeners, `DEFER_TASKRUN` on every ring and bounded reads after a burst are its
  ideas. The `resv[0]` retry is also from c-iouring-pavlyhalim (#245). The WAL writeback kick follows #516.
- asm-btree-ninhnv (#388), for the split: the server in NASM, SQLite's B-tree reached through a small C shim.
  No code from it is used.

## License

MIT, under the repo's [license](../../LICENSE).
