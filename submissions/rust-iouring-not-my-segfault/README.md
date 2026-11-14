# twelve-loop: Rust and C on io_uring, SQLite in-process

One event loop on one thread drives eight io_uring rings and serves the API straight from SQLite in the same process.
Every write is committed before its reply, and the server reads nothing box-wide.

## At a glance

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024) and C (GCC from Ubuntu 24.04's `build-essential`) |
| Framework | None: our own HTTP/1.1 server on io_uring; the `httparse` crate parses request heads |
| SQLite driver | SQLite's C API called from Rust through a small hand-written binding (no wrapper crate) |
| SQLite | 3.54.1, the official amalgamation, compiled unmodified in one C unit with our two C files |
| Crates | `io-uring` 0.7.15, `libc` 0.2.183, `httparse` 1.10.1, `hmac` 0.12.1, `sha2` 0.10.9; exact versions locked in `Cargo.lock`, built with `--locked` |
| Nginx or direct | **Direct.** The server listens on `HOST:PORT` itself (`0.0.0.0:80` on the box) |
| Threads | Two: the event loop, and a watchdog that only prints a line when the loop stalls for 2 s |

## Build and run

- `install.sh`, as root, once: installs `build-essential`, `curl` and `ca-certificates` with apt, then rustup-init 1.28.2, checked against its pinned SHA-256 before it runs, which installs Rust 1.94.0 (minimal profile plus rustfmt and clippy) under `/opt/rust`.
  - It writes small proxies for `cargo`, `rustc` and the other tools into `/usr/local/bin`, so `build.sh` runs as the normal build user without access to root's home (see "Toolchain install" under the rules).
- `build.sh`, as the normal user: downloads SQLite 3.54.1's autoconf tarball, checks its pinned SHA-256, puts `sqlite3.c` and `sqlite3.h` into `vendor/`, then runs `cargo build --release --locked`.
  - `build.rs` refuses any SQLite whose source id is not 3.54.1's, compiles `src/sqlite_unit.c` (the amalgamation, then `readers.c` and `checkpointer.c`) with GCC and every warning an error, and links it statically.
- `start.sh`: replaces itself (`exec`) with `target/release/twelve-loop` in the foreground, with no arguments. The server is configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`, as SPEC.md gives them.

### The two builds

| Build | How | What it is |
|---|---|---|
| Fast (default) | `bash build.sh` | What the contest builds. Rust with `-C target-cpu=native -C target-feature=-avx512f`, opt-level 3, fat LTO, one codegen unit, `panic = "abort"`, overflow checks off; the C unit with `-O3 -march=native -mno-avx512f` and Ubuntu GCC's default hardening switched off (`-fno-stack-protector -U_FORTIFY_SOURCE -fno-stack-clash-protection -fcf-protection=none`). It ships the unsafe paths listed below. |
| Safe | `TWELVE_BUILD=safe bash build.sh` | Follows every rule the fast build's unsafe paths skip: overflow checks on, LTO off with 16 codegen units, frame pointers in Rust and C, and the C hardening flags `-fno-strict-overflow -fno-strict-aliasing -fno-delete-null-pointer-checks -ftrivial-auto-var-init=zero -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection`. Each unsafe path takes its checked form. |

Other switches `build.sh` reads: `RUSTFLAGS` (kept when set; both builds append `-C target-feature=-avx512f`, and the safe build its own flags), `TWELVE_C_TARGET` (the GCC `-march` value, default `native`) and `CC` (default `/usr/bin/gcc`).
No build emits a 512-bit (AVX-512) instruction, whatever the CPU target: a Cascade Lake core lowers its clock while it runs them.
`start.sh` passes no flags, so the server always runs its defaults; the optional flags exist for our own experiments, and the startup line on stderr prints every setting the process runs with.

- `--batch-target N` or `--batch-target=N`: request target 1 through 1,024, default 96; ASCII decimal, leading zeros accepted, repeated last value wins.
- `--ring-spread fill` or `--ring-spread=fill`: exactly `fill` (default) or `spread`, repeated last value wins.
- A missing or invalid value exits 2 with `startup refused: invalid --batch-target` or `startup refused: invalid --ring-spread`, followed by LF.
- The startup line appends `batch_target` and `ring_spread` after `backlog`; the stats line appends cumulative `rearm_buffer_wait` after `linger_errors`.

## How a request is served

- Accept: a multishot accept on one ring at a time installs each new socket straight into that ring's fixed-descriptor table, so a connection costs no process descriptor. Each ring's table is as large as the hard file limit allows, at most 65,535 slots, so eight rings hold up to 524,280 connections.
  - With `fill`, when the accepting ring's free slots fall to h = max(slots / 10, min(1,024, slots / 8)), accept moves to the next ring with more than h free, or pauses until a ring has 2h free.
  - With `spread`, accept moves to the least occupied eligible ring once its owner has more live connections than the least occupied ring plus the turn cap. In-flight accepts during cancellation can exceed that skew; connections never move.
- Receive: a multishot receive per connection into a ring of provided buffers (1,024 buffers of 2 KiB per ring).
  - After a turn that reaped or advanced work, ring 0 waits for the ceiling of its live-connection share of twice `--batch-target` completions, approximately one receive and one send per request, for at most the ceiling `--wake-wait-us=N` sets: 0 to 10,000 µs, default 4,000 µs (4 ms).
  - After a turn that did neither, ring 0 waits for one completion, bounded only by its deadlines: an idle loop, or one whose only work waits on a held sync, does not wake every 4 ms.
  - The 4 ms ceiling also applies while rings 1 to 7 hold connections or operations, since their completions cannot wake ring 0's wait.
  - A receive that ends while its connection has room in its input reservation is re-armed into that reservation, which takes no provided buffer.
  - Any other ended receive, buffer exhaustion included, joins its ring's FIFO. Each round limits buffer-select receive submissions to the free buffers observed after processing receives; a full SQ retains its operation for the next round.
  - `rearm_buffer_wait` counts deferred connection-rounds or retained receive attempts without buffer credit, not unique connections.
- Parse: `httparse` reads the head (at most 64 KiB); bodies by `Content-Length` or chunked, at most 64 KiB; `Expect: 100-continue`, HTTP/1.0, keep-alive and pipelined requests, answered in order.
- Auth: every token is verified on every request: HS256 only, the 43-character signature compared in constant time, `exp` and `nbf` checked, `sub` a positive integer and `username` a string.
- Reads (`GET /feed`, `GET /posts/:id`): inside one SQLite read transaction per turn, through SQLite's own B-tree cursors (`readers.c`), with the SQL statements as the fallback.
- Writes (`POST /posts`, `POST /posts/:id/like`): queued and committed in groups; each reply leaves only after its own `COMMIT` returned.
- A request that meets a full read batch (1,024 a turn) or a full write queue stays on its connection with its receive armed and is admitted on a later turn. Output pressure, a client that does not read its replies, still pauses the connection and cancels its receive.
- Send: each reply is written into the connection's output buffer with room kept in front for the head, then handed to the ring as a Send operation.
- Every 10 s the server prints one `stats` line of counters on stderr.

## Optimizations, and why

### One thread, eight io_uring rings

- On one vCPU a second worker thread only adds context switches and locks, so one loop batches its system calls: each round enters active nonzero rings once, then submits ring 0 and waits in one enter. Queued completions do not bypass batching. Ready backlog and eligible queued work use zero timeout; deadlines can shorten the 4 ms ceiling.
- Fixed (direct) descriptors skip the process file table on every operation; provided buffer rings let a receive land in a shared 2 KiB buffer instead of one buffer per idle connection.
- Eight rings, because one ring's fixed-descriptor table can be no larger than the file limit: eight tables cover more connections than the box has memory for.

### SQLite reads through its own B-tree cursors

- `GET /feed` and `GET /posts/:id` skip SQL parsing and bytecode: `readers.c` opens SQLite's own read-only B-tree cursors on the tables and indexes, inside the same open SQL read transaction, and decodes the record bytes SQLite's payload functions return. The cursors stay open across a turn's reads and close before the turn's `COMMIT`.
- The SQL statements stay as the fallback and answer every read the cursors decline: no open transaction, a schema change since open, a feed span of 256 or more ids, a missing row or author, or a record it cannot decode. At open, the readers check the two indexes' shapes and stay off when they differ.
- The optional `--check-reads` flag compares each fast-path answer with the SQL answer when both exist; fallbacks, and every read while the readers are off, run SQL only.

### Group commit with an adaptive window

- Writes queue (up to 4,096) and commit in groups of up to 64 in one transaction: when 64 are waiting, when the oldest has waited the window, or on a quiet turn with nothing else to do. A quiet turn received at most one request, or saw no write join the queue.
  - Under load the window decides, so a commit carries the writes of several turns and writes each page they share once: fewer WAL frames, `pwrite` calls and checkpoint passes per write. A lone write at low load commits in the turn that received it.
- The window moves between 4 and 10 ms every 250 ms, from the server's own CPU time per request and its write latency.
- A failed commit rolls back, and every write of that group that changed the database is answered 500; no reply leaves before its commit.

### Checkpoints that never stall the loop

- Automatic checkpoints are off. A second SQLite connection runs PASSIVE checkpoints on a fiber inside the loop thread, through a VFS shim over SQLite's `unix-excl` VFS registered with `sqlite3_vfs_register`.
- Each sync SQLite asks for during a checkpoint is submitted as an io_uring fsync; the loop keeps serving until it completes, then the fiber resumes with the real result. Only the connection's first WAL sync, with its one-time directory sync, runs inline. Page writes yield to the loop every 1 ms.
- A checkpoint cycle is two passes. A bulk pass starts at every 4,096 new frames and copies the WAL while writes go on. When it ends with frames committed beside it, new writes wait (reads continue) while a tail pass copies only those frames and makes the cycle's database sync; the next write then restarts the WAL.
  - SQLite syncs the database file only in a pass that reaches the WAL's end. The bulk pass, with commits beside it, syncs the WAL and writes its pages; the held tail pass's database sync puts the whole cycle's pages on disk.
  - A tail hold lasts 250 ms at most, half the contest's p95 bar: if the tail pass's syncs take longer, writes go on, the pass ends uncovered, and the next cycle starts with an unheld pass.
  - The backstop is the write gate: at 32,768 frames (128 MiB) new writes wait without a time limit until a pass has copied the whole WAL.

### The close path: reads before closes, nothing allocated, reset when safe

- When a client hangs up, the server reads first: in a turn that received data, at most 64 closes start; otherwise up to 4,096 a ring, which keeps a turn's close completions within the completion queue's room.
- A close with nothing in flight keeps its state in the connection's own 64-byte record: it allocates nothing and sends no cancel.
- Before the close the server asks the kernel how many sent bytes are unacknowledged (`SIOCOUTQ`). At 0 every reply byte has arrived, so the close is abortive (zero linger) and the socket's memory is freed at once; otherwise linger is turned off first and the close is the normal graceful one, which keeps the kernel sending the unacknowledged bytes.
  - A client that keeps sending after the server's close can still lose that last reply to the reset its data provokes (RFC 9112, section 9.6, "Tear-down"); the safe build drains instead (the "No orderly close" row under "Unsafe paths in the fast build").
- Zero linger is set once on the listening socket, after a startup check that accepted sockets inherit it, so an abortive close is a single Close operation.

### Bounded memory

- Each connection is one 64-byte record in one table allocated zeroed, so slots never used cost no memory.
- All connections share an input budget of 16 MiB and an output budget of 32 MiB; at most 1,024 freed buffers of each kind are kept for reuse.
- SQLite's own page cache is 32 MiB, with no memory map; statements are prepared once and reused.

### Smaller things

- Requests and replies reuse per-connection and per-loop buffers. JWT base64url decoding recognizes common flat ASCII JSON during decoding; escapes, Unicode and nested values use the full validator. HMAC clones the library's precomputed key state.
- Token expiry derives wall seconds from monotonic time plus a UTC offset refreshed every 100 ms. The write window reads a fresh completion clock only at its 250 ms adaptation boundary; counters are gathered every 100 ms and before each stats line.
- Each connection's record is read for its flags alone wherever the flags decide, and prefetched when its completion is reaped. Its metadata cell index lives inside the same 64-byte record, without a separate lookup allocation. A complete unauthenticated read with credit enters the read batch directly.
- A create reads its new row's `id` and `created_at` from the insert itself (`INSERT ... RETURNING`); temporary tables live in memory (`PRAGMA temp_store = MEMORY`).
- JSON escaping takes 16 bytes at a time with SSE2 (in Rust for replies, in C inside the readers). The readers load 16 text bytes, store them to the output and test the loaded bytes in one step; a text's last bytes take one more step over its final 16.
- A feed of contiguous ids writes each row straight into the output: before a row it checks room for the row's fixed bytes plus six times its text bytes, then stores without per-fragment checks. A feed of scattered ids reads its rows in id order, encodes their text once into reused scratch and copies the fragments in feed order.
- Input reservations keep their actual allocation charged rather than shrinking it. Pools search newest-first for sufficient capacity, and cached usernames retain bounded slack rather than shrinking on each request.
- SQLite is compiled with `SQLITE_THREADSAFE=0`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_DQS=0`, `HAVE_FDATASYNC=1`, `SQLITE_ENABLE_QPSG`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_OMIT_DEPRECATED`, `SQLITE_OMIT_SHARED_CACHE`, `SQLITE_OMIT_PROGRESS_CALLBACK` and `SQLITE_LIKE_DOESNT_MATCH_BLOBS`.
- Fat LTO, one codegen unit and the native CPU target without 512-bit instructions carry no benchmark of their own.

## The rules, in the organizer's words

### SQLite, through its own interface

- "**The database is SQLite**: the file at `$SQLITE_PATH`, used in-process." (README, rule 2)
- The organizer's rulings (LEADERBOARD.md, "Arjay's rulings on SQLite (2026-10-31)"): "Reading `/feed` and `/posts/:id` through SQLite's own B-tree/cursor API is allowed (rules 2/3)", and so are "SQLite's public plugin APIs (a custom page cache through `SQLITE_CONFIG_PCACHE2`, a custom VFS)". Not allowed: "**an own engine for SQLite's file format**" and "**patching SQLite's source**".
- This server reaches the database only through SQLite:
  - `src/sqlite_unit.c` includes the official 3.54.1 amalgamation unmodified; `build.sh` checks the tarball's SHA-256 and `build.rs` its source id.
  - Writes and transactions are prepared SQL run by `sqlite3_step`.
  - The fast reads use SQLite's own B-tree cursors, transaction state and payload functions inside SQLite's open read transaction; SQLite's pager, page cache, WAL and B-tree traversal do all the work below them. `readers.c` makes no file call of its own (no open, read, write or mmap) and never writes to the database.
  - The checkpointer is a VFS registered with the public `sqlite3_vfs_register` that forwards every file call to SQLite's own `unix-excl`; the checkpoint itself is `sqlite3_wal_checkpoint_v2`.
- Rule 3 allows "Any SQL you like against the schema as given": no DDL runs, and the schema and data stay as given.

### Writes are durable before the reply

- "**Writes are durable before you respond.** A 201 means the row is committed. Use WAL with `synchronous=NORMAL` or stronger" and "Batching several requests' writes into one transaction (group commit) is allowed if each response waits for its commit." (README, rule 6)
- Both connections run `journal_mode=WAL` with `synchronous=NORMAL`. A write's reply is released only after the `COMMIT` of its group returned; a failed commit rolls back and answers 500. Every fsync SQLite asks for is performed and its real result returned to SQLite.

### No caching across requests

- "Every request reads its data from SQLite while it is being served" and "SQLite's own page cache and `mmap` are fine. That's the database." and "Reusing prepared statements is fine too: they cache the query plan, not the data." (README, rule 5)
- Every read runs inside a read transaction opened in the turn that serves it. The readers keep only root page numbers, key descriptors and cursor memory between requests, never a row, an id, a count or a reply. Every token's signature is verified on every request.

### Nothing tuned on the box, nothing read about it

- "11. Nothing on the box gets tuned for you: no kernel parameters, and no pinning of CPU or other processes." and "Settings inside your own process (GC, thread counts, allocator, pragmas allowed by rule 6) are fair game." (README, rule 11)
- The organizer flagged entries that react to box-wide memory or sockets: one "sheds load" "based on /proc/meminfo", another "reads the box-wide `/proc/self/net/sockstat`" (LEADERBOARD.md, "Rule flags from code review").
- The shipped server opens no `/proc` or `/sys` path, sets no kernel parameter and pins nothing. What it asks the kernel: its own file limit (it raises its soft limit to its hard limit), the kernel release and the io_uring operations it supports (printed in the startup line), its own CPU time (for the write window), and its own sockets' unacknowledged bytes (`SIOCOUTQ`, for the close).
- It sheds nothing on memory and has no idle timeout and no connection cap: an idle keep-alive connection stays open; only a connection holding input, output or an operation that has waited 120 s on its client is closed.

### The close path follows the server's own state

- "7. No hard-coded responses, no detecting the load test, nothing that only works because it's a benchmark." (README, rule 7)
- "Ordering routes or fast paths by observed traffic is fine too (rule 7), as long as every route works for any request" (LEADERBOARD.md, "Arjay's rulings on SQLite (2026-10-31)")
- The organizer flagged an entry that evicts connections, "tuned to the load test's order (rule 7 question)" (LEADERBOARD.md, "Rule flags from code review"). This server evicts no connection at any count.
- The close rule is the same in every turn, under load and idle: a close starts after the client hangs up, reads come before closes, and the reset is chosen only from the kernel's answer for that one socket. It keys on nothing but the server's own completions, and has no threshold on hang-ups and no memory figure.

### Memory and connections

- "If you run out of memory, you lose." (README, rule 8) Every queue, buffer and pool has a fixed limit (see "Bounded memory").
- "your server has to handle up to ~15,000 open keep-alive connections by itself" (README, rule 9): with a hard file limit of 65,535, the eight rings hold up to 524,280 connections, each in a 64-byte record.

### Toolchain install

- "It is built from source on the box with your `install.sh` and `build.sh`." (README, rule 10)
- The organizer's flags list "**Unchecksummed toolchain downloads in install/build**" and name "rustup (#82, also writes /usr/local/bin wrappers)" (LEADERBOARD.md, "Rule flags from code review").
- `install.sh` downloads rustup-init 1.28.2 from `static.rust-lang.org`'s versioned archive and checks it against its pinned SHA-256 (one per architecture) before running it; rustup then installs the pinned toolchain 1.94.0. `build.sh` checks SQLite's tarball against its pinned SHA-256, and Cargo checks every crate against `Cargo.lock`.
- The proxies in `/usr/local/bin` are small shell scripts that set `RUSTUP_HOME=/opt/rust/rustup` and `exec` the tool from `/opt/rust/cargo/bin`. The toolchain lives in `/opt/rust`, readable by every user, so `build.sh` runs as the normal build user with `cargo` on its default `PATH` and no access to root's home.

### start.sh

- The organizer flagged "**start.sh restart loops** (the app restarts itself instead of letting systemd see a crash)" (LEADERBOARD.md, "Rule flags from code review"). `start.sh` replaces itself with the server (`exec`) and loops nowhere; a crash ends the process.

## Unsafe paths in the fast build

The fast build ships these places openly as unsafe paths, for speed.
Each one carries an `UNSAFE PATH (speed)` comment in the code that names what it skips and the invariant that keeps it correct.
The safe build (`TWELVE_BUILD=safe`) follows the rule each one skips.

| Group | Places | What it skips | Why the fast build skips it | What the safe build does |
|---|---|---|---|---|
| Unchecked arithmetic | 47: `src/conn.rs` 27, `src/server.rs` 15, `src/ring.rs` 5 | Checked or overflow-trapping integer arithmetic | Each plain operation sits under a comment naming the bound that keeps it in range: counts bounded by the 524,280 records, offsets within a buffer's start, length and capacity, budget sums at most 32 MiB, in-flight counts bounded by live kernel requests | Overflow checks on: any overflow aborts the process |
| Aggregate output bounds | 3: `src/readers.c` | CX20 per-fragment bounds and checked output-position addition | A whole-feed, whole-row or whole-string reservation proves every fragment fits, a number's fixed 20-byte copy included; at most 20 SQLite records expand at most sixfold below SIZE_MAX on the asserted 64-bit target | Checks every fragment and output-position addition |
| Unchecked narrowing casts | 4: `src/server.rs` 3, `src/readers.c` 1 | `try_from` or a checked helper for a narrowing conversion | The value was bounded just above: a receive result of at most 2,048 bytes, a feed slot below 20 | `try_from` with a stop on failure in Rust; an active range assert in C |
| Vector code without a benchmark | 2: `src/json.rs` 1, `src/readers.c` 1 | No `unsafe` or vector code without a recorded benchmark | Each 16-byte SSE2 load reads only bytes of the text it copies, after a check that 16 remain or that the text's last 16 are being taken, needs no alignment, and SSE2 is part of every x86-64 CPU | The plain byte loop; no vector code |
| Varint reads | 2: `src/readers.c` | A length check on every varint read of a record header | SQLite's varint readers read at most 9 bytes from a start below the header end, inside the cell's page image, and a header that runs past the payload is refused before any byte past it is used | A bounded varint decoder that checks every byte |
| `errno` | 2: `src/readers.c` | Saving and restoring the caller's `errno` in the reader entry points | No caller reads `errno` after a reader call | Saves and restores it |
| Precondition asserts | 2: `src/readers.c` | Asserting each entry point's preconditions | Both callers call the readers only after a successful open, on that connection, with their own buffers | The asserts run |
| No orderly close | 6: `src/server.rs` 4, `src/ring.rs` 1, `src/main.rs` 1 | The orderly close: half-close, read to the peer's end of stream, then close | A reset only when `SIOCOUTQ` says every reply byte is acknowledged, otherwise a graceful close; this frees socket memory at once when many clients hang up together | No linger change and no `SIOCOUTQ`; an intentional close stops serving, keeps receiving and discards what arrives, half-closes the write side and closes at the peer's end of stream |
| Build settings | 6: `Cargo.toml` 3, `build.rs` 3 | Frame pointers in Rust and C, overflow checks, a benchmark beside fat LTO and one codegen unit, the C hardening flags, and the hardening Ubuntu's GCC turns on by default (stack protector, `_FORTIFY_SOURCE`, stack-clash and control-flow protection) | A frame pointer takes a register from every function and line tables still map addresses to source lines; LTO and codegen units change only inlining and layout; the hardening flags and GCC's defaults add code to SQLite's hot paths, and our C is written to be correct without them | Frame pointers, overflow checks, LTO off with 16 codegen units, the hardening flags; GCC's defaults stay on |
| Forced inlining | 1: `src/readers.c` | An inlining hint only with a measured effect | `out_put` costs 0.127 µs and its copy 0.128 µs a request as calls of their own (`RESULT.md` → "Speed profile at 310,000"); inlined into each writer, a literal fragment's copy has a constant length, and the bytes written are the same | No inlining attribute; the compiler decides |
| First-field record decode | 1: `src/readers.c` | Decoding a likes index entry's whole record header before reading a field | A one-byte header size and a one-byte first serial type locate the first field, the one value the scans read, and the field is checked to lie inside the payload; the whole-header decode costs 0.153 µs a request | Decodes the whole header (`record_read`) first |
| UTF-8 checked once | 1: `src/server.rs` | A UTF-8 check where a write's name and body are read | Admitting the write checked the same span of the same input, and nothing rewrites it before the reply is released; `from_utf8` costs 0.021 µs a request | Checks it again with `from_utf8` |
| Cache prefetch | 1: `src/server.rs` | No `unsafe` code without a recorded benchmark | A prefetch never faults and changes no memory, and its address lies inside the live connection table; `Conns::get` costs 0.216 µs a request | No prefetch |

All 78 `UNSAFE PATH` comments in the code fall into these thirteen groups.
