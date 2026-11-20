# Fortran + io_uring + SQLite

One thread of Fortran 2018 on io_uring, with SQLite compiled into a small C shim. Direct on port 80.

| | |
|---|---|
| Language | Fortran 2018, GNU Fortran 13.3 from Ubuntu 24.04's own `gfortran` package, `-O3 -march=native -fwrapv` |
| Framework | none: hand-written HTTP/1.1, JSON, JWT and SHA-256 in Fortran |
| io_uring | liburing 2.15, built from the pinned tarball; needs Linux 6.1+ |
| SQLite | 3.54.1 amalgamation, SHA-256 pinned, compiled into `src/shim.c` with GCC 13 (`-O3 -march=native`), not patched |
| C | `src/shim.c`, 632 lines: wrappers for liburing's inline prep helpers, SQLite's file-private B-tree calls, a checkpoint thread, the code lock. No request logic |
| **Nginx or direct** | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box) |

This is the same design as [mojo-iouring-oh-meh](../mojo-iouring-oh-meh) (same author), ported
line for line to the oldest language on the board. The point of the exercise: the top of this
leaderboard is one design in a dozen languages within a few percent of each other, and the
measurements say why: on the benchmark box about 20 of the 26 µs a request costs are the kernel's,
and about 3.6 KB of the 4 KB a connection costs are the kernel's socket. What a language can add
or take away is the remaining fifth. So this entry asks the question the other way round: can a
1957 language hold the 2026 design with only plumbing in C? The hot path (parsing, auth, SHA-256,
JSON, record decoding, the query plans, the event loop, the bounded-read recovery) is Fortran.
The C shim is the one from that entry's follow-up PR #319 (`396de83`, bounded reads after a burst), unchanged; the merged Mojo folder on `main` is the earlier #257 revision, so the two shims differ until #319 lands.

## Running it

```bash
sudo bash install.sh   # build-essential and gfortran from apt (about 30 s)
bash build.sh          # fetches + verifies SQLite and liburing, builds bin/server (about a minute)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` runs in the foreground and reads only the SPEC.md variables (plus
`FORTRAN_TABLE_OVERRIDE`, `FORTRAN_NBUFS_OVERRIDE` and `FORTRAN_WAL_HOLD_OVERRIDE`, test-only knobs
for forcing tiny descriptor tables, few provided buffers and early WAL holds; never set on the box).

## How Fortran does this

- **Memory it does not own.** Every request-path byte lives in C-owned memory: io_uring's provided
  buffers, SQLite's pages, arenas allocated once at startup. Fortran addresses it as
  `integer(c_intptr_t)` and lays a `c_f_pointer` view (a contiguous, 0-based `integer(1)` array)
  over a region when it reads or writes it. Bytes are signed in Fortran, so comparisons above 127
  go through an unsigned helper. No Fortran `character` strings are on the hot path; literals are
  compared and emitted byte by byte.
- **Integers wrap.** SHA-256's additions rely on two's-complement wrap-around of `integer(4)`,
  which the standard leaves undefined; the build passes `-fwrapv` outside the overridable flags
  so GCC makes it defined, and `bin/selftest` (built and run by `build.sh`) checks the SHA-256
  and HMAC test vectors so a lost flag fails the build, not the tokens. Shifts are the logical
  `shiftl`/`shiftr`. The modules are compiled with `-flto`: without it every byte helper in
  `bytes.f90` is an out-of-line call from the parser and the record decoder.
- **State is module state.** Each Mojo struct became a module with its variables and procedures
  (`srv`, `db`, `jwt`, `sha256`, `resp`); the connection table is a `calloc`ed array of 64-byte
  `bind(C)` records viewed through a pointer, so untouched slots cost no memory. Growable lists are
  allocatable arrays doubled by `move_alloc`.
- **C interop** is `bind(C)` interfaces in `src/shim.f90`, one per shim function, with `type(c_ptr)`
  by value where C takes `void*` and `character(kind=c_char), dimension(*)` for NUL-terminated
  strings. Nothing else is needed: no preprocessor, no ISO_C_BINDING tricks beyond `c_f_pointer`.

## Design

The design is the Mojo entry's; its README has the long form and the measurements behind each
choice. In short:

- **One thread.** The event loop, HTTP, JWT, JSON and SQLite reads all run on one thread. A second
  C thread only runs passive WAL checkpoints, at nice 19. SQLite is built `THREADSAFE=2` for that.
- **Rings.** Accepted sockets are io_uring direct descriptors, so `LimitNOFILE=65535` caps one
  ring's table, not the server. Rings open lazily, each with `RLIMIT_NOFILE - 64` slots, up to 16.
  Flags: `SINGLE_ISSUER`, `DEFER_TASKRUN`, `TASKRUN_FLAG`, `CQSIZE` (65,536 completions per ring),
  `SUBMIT_ALL`.
- **Accept** is one-shot direct accepts (`IORING_FILE_INDEX_ALLOC`), up to 512 in flight over eight
  `SO_REUSEPORT` listeners, never more than the current ring has free slots; accepting moves to the
  next ring when one fills.
- **Receive** is multishot into provided buffers, 2,048 × 2 KB per ring, parsed in place; the
  buffer goes back at the end of the batch. Idle connections hold no buffer.
- **After a stall, reads are bounded and drained first** (the design of
  [rust-iouring-kensac](../rust-iouring-kensac)): a receive that ends with `ENOBUFS` is not
  re-armed but queued, and each pass issues one single-shot `MSG_DONTWAIT` receive per queued
  connection, never more than the ring's free buffers; a pass reads up to 16,384 times and answers
  at most 2,048 requests, parking the rest in the connection's buffer on a ready list served oldest
  first. The multishot receive is armed again only after the pass has handed its buffers back.
- **Send** is one `SEND` per response from an arena the batch writes into. Pipelined requests are
  answered in order, one send in flight per connection.
- **Batches.** Everything from one wake-up is one batch: reads are answered from one `BEGIN` with
  B-tree cursors opened once per batch, then writes run as prepared statements, then `COMMIT`, then
  the sends go out. A write's response is built only after the commit and holds its place in the
  connection's response queue from the moment it is parsed. After a busy pass the next wait asks
  for 32 completions or 250 µs.
- **The WAL** is kept short by restarts: past 8,192 frames the loop holds its writes until the
  checkpoint thread reports the log backfilled (at most 1 s), and the next write restarts it.
- **Closing** is a `SHUTDOWN` hard-linked to the direct-descriptor `CLOSE`. The peer's EOF is
  remembered and the connection closes once nothing is parked, queued or in flight. Healthy
  keep-alive connections are never closed by the server.

## SQLite

- Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `cache_size=-32768` (32 MiB), `mmap_size=0`,
  `temp_store=MEMORY`, `wal_autocheckpoint=0`. VFS `unix-excl`, so the WAL index is in heap memory
  and a transaction takes no fcntl lock.
- **Reads walk the B-tree.** `GET /feed` is the last 20 rows of the posts tree by rowid, one range
  scan of `likes_post_id_idx` for all 20 like counts, and a rowid seek into `users` per row.
  `GET /posts/:id` is a rowid seek plus the same two lookups. The calls are SQLite's own
  (`sqlite3BtreeTableMoveto`, `IndexMoveto`, `Next`, `Previous`, `PayloadFetch`), through its
  pager, WAL and locking, inside the batch's transaction; only the bytecode engine is skipped, as
  the organizer allowed for the Lean entry. Record decoding is `src/record.f90`.
- The feed plan is only right when post ids are dense and `created_at` never decreases with id.
  The server checks both with SQL at startup, and after every insert by reading the previous row
  back from SQLite and comparing it with the `RETURNING` values. The only thing kept between
  requests is one boolean saying which plan to use; it flips to the reference SQL query for good
  if a check ever fails. No data is remembered across requests.
- **Writes** are the reference `INSERT ... RETURNING` and `INSERT ... ON CONFLICT DO NOTHING`,
  group-committed per batch; every 201 waits for its `COMMIT`.

## Auth and HTTP

- HS256 only; `exp` required and checked against the batch's clock; `sub` must be a positive
  integer string; `username` is used as-is. HMAC-SHA256 is in `src/sha256.f90` with the key's
  inner and outer pad states hashed once at startup; every token's signature is checked on every
  request.
- The parser reads the request line, `Authorization`, `Content-Length` and `Connection`; everything
  else is skipped. Requests that span receives are completed in a per-connection buffer that grows
  by `realloc` from 512 bytes to a 16 KB cap (413 past it; parked complete requests may reach
  256 KB). HTTP/1.0 or `Connection: close` closes after the response.

## Measurements

All on the arm64 development VM (4 vCPU Ubuntu 24.04, kernel 6.8.0-147 like the box's, k6 and the
generators over loopback). Nothing has been run on x86 yet.

- `test/run.sh`: 42/42.
- Lifecycle probes (fragmented POST, mixed pipelines, half-close, pipelined `Connection: close`,
  WAL hold then silence): all pass; a POST during a hold is answered in 44 ms with no other traffic.
- Stall probe with exact accounting (every connection sends while the server is `SIGSTOP`ped;
  every third half-closes, every fifth sends a like ahead of a GET, every seventh a POST then a
  `Connection: close` GET then trailing bytes; responses are identified by post id): 6,000
  connections, 3,000 with one pipelining 2,501 requests, and 900 over 16 rings of 64 slots with
  16 buffers each: ALL EXACT, first reply 55-61 ms after `SIGCONT`, everything answered within
  0.24 s.
- Saturation (k6, 500 connections, no think time, 20 s, three runs each, alternating): 76,000-82,000
  req/s at 4.6-5.0 µs of server CPU per request, 23 MB RSS, against 74,000-84,000 req/s at
  4.1-4.8 µs for the Mojo entry. The spread between runs (the laptop under load) is as wide as the
  gap between the two; at the box's 26 µs per request either way the language is a few percent.
- The benchmark's user model at the box's shape: the server confined to 10 % of one core (which
  makes a request cost 27 µs, the box's figure), 40,000 users with the think times at 0.75×,
  three generator processes on the other cores, 30 s ramp then 180 s hold, both entries under the
  same conditions minutes apart:

| | share of the slice | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| this entry | 96-98 % | 140 ms | 649 ms | 830 ms | 1.9 s |
| mojo-iouring-oh-meh (#319 tree) | 93-95 % | 66 ms | 355 ms | 568 ms | 1.1 s |

  The Fortran build sat about 3 points closer to saturation, and at that utilisation the tail is
  steep, so this is the cost of the per-request difference above, not a design difference. A
  second pair at 90 % of the slice was disturbed by a 20 s pause of the whole VM in both runs
  (the host swapping) and is not reported. 0 errors in every run.

## Honesty notes

- This is a port, not a second design: the Mojo entry's loop, plans and recovery logic (as of
  its PR #319) were translated procedure by procedure, and that revision's C shim is reused
  unchanged. The language is the only variable. Both entries were written with an AI assistant and reviewed by a second model; the
  defects that review found in the Mojo loop (listed in its README) are fixed here too because the
  code was ported after them.
- Ubuntu's 6.8 kernels reject `IORING_REGISTER_PBUF_RING`, so this uses `IORING_OP_PROVIDE_BUFFERS`.
- The multi-ring path was exercised with 64-slot tables, not by filling a 65,471-slot table with
  real connections. Nothing above 40,000 simultaneous connections has been run.
- The `-fwrapv` dependence is real: without it the SHA-256 compression function is undefined
  behaviour in standard Fortran, and GCC may use that at `-O3`. The JSON writer scans a byte at a
  time where the Mojo scans eight (SWAR); the output is identical.
- Independent review (a second model, read-only) of the port against the Mojo source found one
  transcription defect: the feed plan read `bt_last`'s out-argument in the same logical
  expression as the call, which Fortran does not sequence (the same trap had already bitten the
  self-test once). Every other out-argument site was then checked; that was the only one. Fixed
  before the PR. The review found no mismatch in the lifecycle paths, the C bindings or the
  build scripts. A second pass over the structural clean-up that followed (one post-object
  writer, a node record for the response queue, shared ring helpers, a statement array) found
  no blocker and one ordering nit, restored.
- gfortran starts no threads of its own; the only second thread is the shim's checkpointer.

## Layout

```
src/server.f90   event loop, rings, connection table, batches, routing (module srv + program)
src/db.f90       transactions, feed/post plans on B-tree cursors, writes
src/http.f90     request parser
src/jwt.f90      base64url, HS256 verification
src/sha256.f90   SHA-256 and HMAC
src/json.f90     JSON validation, the "body" field, JWT payload fields
src/record.f90   SQLite record format
src/resp.f90     response arenas and headers
src/bytes.f90    raw byte helpers, the writer, JSON escaping
src/shim.f90     bind(C) interfaces to the shim and libc
src/selftest.f90 unit checks for the leaf modules (built and run by build.sh)
src/shim.c       the C shim: liburing helpers, SQLite B-tree calls, checkpoint thread, code lock
```
