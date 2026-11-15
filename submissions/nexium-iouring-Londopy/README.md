# nexium-iouring-Londopy

**[Nexium](https://github.com/Londopy/nexium) on raw io_uring and SQLite without SQL: the whole server is written in a young language that compiles through C, and it was tested function by function against a C original.**

Nexium is a systems language with checked effects, bounds-checked slices and three build modes, and its compiler `nx` emits C. This entry is a port of [`c-iouring-Londopy`](../c-iouring-Londopy) (pull request [#438](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/438)): the same design, the same constants, the same bytes on the wire. The question it answers is whether such a language can sit this close to the kernel without paying for it. Two things set the entry apart:

- **No C in the server.** The event loop, the io_uring plumbing, HTTP, JSON, JWT (SHA-256 and HMAC included), SQLite's record format, the B-tree reads and writes, the group commit and the checkpoint thread are 5,300 lines of Nexium in 11 modules. It reaches the kernel through `syscall()` and maps the rings itself: no liburing, and no wrapper library of its own.
- **Every function has a C twin, and the two were run side by side.** The protocol code agrees with the C on 33.6 million generated cases per build mode, the storage layer leaves byte-identical databases, and the whole server answers a 2,500-request write script exactly as the C server does. See "How it was tested".

| | |
|---|---|
| Language | Nexium 1.5.0. `install.sh` builds its compiler, `nx`, from source at a pinned commit with gcc alone; `nx` then compiles the server through gcc from Ubuntu 24.04's `build-essential` |
| Framework | none: raw `io_uring` syscalls (no liburing) |
| Database | SQLite 3.54.1 amalgamation, compiled in (SHA-256 pinned in `build.sh`) |
| Front | **Front: direct** (serves `0.0.0.0:80` itself; no Nginx) |
| Author | Londopy |

## What is Nexium here, and what is C

Everything the server does is Nexium. The C that remains is this:

- **SQLite itself**, the pinned amalgamation, as the challenge requires.
- **`src/sqlite_shim.c`, 103 lines.** The server reads and writes SQLite's B-trees directly, and those functions are private to `sqlite3.c`. The shim includes the amalgamation and exports 39 wrappers, each one call or one field of a private struct. None of them loops, decodes or decides: the cursors' use, the record format and every constraint check are in `src/db.nx` and `src/records.nx`. Two wrappers do slightly more, because nx 1.5.0 can't hand a Nexium function to C as a callback or see a private struct: the WAL hook stores two counters, and the insert wrapper fills SQLite's payload struct before its one call.
- **`src/ring_atomic.h`, 16 lines.** Two `static inline` functions: an atomic AND on a 32-bit word the kernel also writes, and a full memory fence. Nexium 1.5.0's atomics are 64-bit only, and a 64-bit operation there would overwrite the neighbouring field.
- **libc, called directly**: `syscall`, `mmap`, the socket calls, and glibc's `memchr`, `memmem` and `strncasecmp` for byte scans, as the C entry uses them.

## How it works

The design is the C entry's, and [its README](../c-iouring-Londopy/README.md) explains each part and why it is there. In short:

- **One thread, eight rings.** Each ring has its own `SO_REUSEPORT` listener and a table of 65,535 direct descriptors. Multishot accept and multishot receive on a shared pool of provided buffers, one send in flight per connection, and one eventfd that wakes the idle loop.
- **SQLite with no SQL on any request.** Reads position SQLite's own cursors on the tables and indexes and decode the records in place. A new post or like is written into its table and every index on it, in SQLite's record format, inside SQLite's own transaction and WAL. The only SQL in normal operation is the `BEGIN IMMEDIATE` and `COMMIT` around each batch of writes. `WRITES=sql` uses the reference `INSERT` statements instead, and `VERIFY=1` checks every B-tree read against the reference SQL.
- **Group commit.** Writes are queued and committed together, at most every 5 ms, and each response goes out only after `COMMIT`. WAL with `synchronous=NORMAL`. A low-priority thread checkpoints the log.
- **A nap the server tunes itself.** A busy pass pauses briefly so completions and sends go through the kernel in batches. The server measures how busy it is and how many requests each pass collects, and picks the pause: none when quiet, up to 3 ms at medium load, none at saturation.
- **Drain first, answer in order.** After a stall, sockets are emptied in bounded rounds into small buffers, and a pass answers at most 2,048 requests, oldest first, so waiting requests don't sit in kernel memory.
- **The Ubuntu 6.8 kernel's quirks are handled**: the inverted reserved-field check when registering a buffer ring (LP #2162843), the ring that can't be allocated on fragmented memory, the `CLOSE` that sends no FIN, and the task-work flag that stays set.

## How it was tested

A port is only as good as its proof that nothing changed, so each part was run against the C it came from, with the same inputs on both sides.

| Part | Compared with the C on | Result |
|---|---|---|
| JSON reader and writers, HTTP parsing, JWT, SHA-256 and HMAC | 33.6 million generated cases per build mode: every return code, every output byte | 0 differences |
| SQLite record format | about 7 million random records per build mode | 0 differences |
| Storage layer: feed, single post, inserts on both write paths, rollback | the same operation script on fresh copies of the seed database | identical output, and byte-identical databases after a checkpoint |
| Event loop: pipelining, half-close, slow readers, malformed input, buffer-pool growth | 26 byte scripts sent to both servers | identical bytes and connection ends |
| The nap controller | 27,371 scripted passes through both implementations | identical state, to the last bit of each double |
| Whole server | the challenge's 42 checks, in 12 combinations of build mode, buffer mode and write path | 42 of 42 in each |
| Whole server against the C server | a 2,500-request write script, then both databases | 0 differences |

The tests ran in all three of Nexium's build modes. `debug` and `safe` keep bounds, overflow and cast checks, so an index out of range traps instead of corrupting memory; nothing trapped. `fast`, which this entry ships, drops those checks: the generated C for the request path has no bounds checks, no reference counting and no allocation. To check the tests themselves, 33 deliberate bugs were planted in copies of the protocol modules, and all 33 were caught.

The functions that never call C carry `!allocates !blocks` bounds, which the compiler checks: JSON, SHA-256, HMAC, base64 and the parsing helpers are proven not to allocate or block. The bound stops at any call into C, so it does not yet cover the path through `syscall` and SQLite.

## Speed against the C entry

Nexium compiles to the same instructions where the algorithm is the same. Measured in fast mode against the C entry's own code, on the same inputs:

| | Nexium / C |
|---|---|
| JWT HS256 verify | 1.00 |
| HTTP request parsing | 1.03 |
| One post as JSON, from the B-tree | 0.99 |
| The feed as JSON, from the B-tree | 1.13 |
| JSON escaping against the C's scalar path (its AVX2 path) | 1.12 (2.29) |
| The event loop over loopback, same work in both | 0.99 |

The one structural gap is JSON escaping: Nexium 1.5.0 has no SIMD, so it checks 8 bytes at a time where the C checks 32, and that is most of the feed's 13%.

Under load, on a replica of the box (Ubuntu 24.04, kernel 6.8.0-147, 1 vCPU, 2 GB, under QEMU), the two entries were run interleaved at 300,000 and 320,000 users, 120 s each, twice. Both held every connection with no errors. The whole guest spent 21–25 µs of CPU per request on the Nexium server and 19–25 µs on the C server, and peak memory was 1.19–1.22 GB against 1.04–1.23 GB. The share of CPU spent in the server's own code was the same for both, about 11%: at this scale the time goes to the kernel's TCP stack. Identical runs on that host differ by more than the two entries do, so the honest summary is that they are about the same, not that either is ahead.

## Configuration

`start.sh` needs only the challenge's variables (`SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`). The optional tuning variables are the C entry's, with the same defaults:

| Variable | Default | What it does |
|---|---|---|
| `RINGS` | `8` | io_uring rings (65,535 connections each) |
| `CKPT_FRAMES` | `2048` | WAL frames before the checkpoint thread starts a pass |
| `HOLD_FRAMES` | `8192` | WAL frames past which writes wait for the checkpoint |
| `HOLD_MS` | `250` | longest a write waits for a checkpoint |
| `COMMIT_US` | `5000` | minimum time between group commits (0 = every loop pass) |
| `CACHE_KIB` | `8192` | SQLite page cache in KiB |
| `PACE_US` | `auto` | the pause after a busy loop pass: `auto` tunes it, a number fixes it in µs, `0` turns it off |
| `WRITES` | `btree` | `sql` = write with the reference `INSERT` statements instead of straight into the B-tree |
| `LEGACY_BUFS` | `0` | `1` = skip the buffer ring and use legacy provided buffers |
| `TCP_CC` | kernel default | congestion control set on the listeners (inherited by connections) |
| `VERIFY` | `0` | `1` = check every B-tree read against the reference SQL (testing only) |
| `DEBUG` | `0` | `1` = log connection errors and 404/500s, plus counters every 5 s |

## Building from source

```bash
sudo bash install.sh   # build-essential, curl, git, and nx 1.5.0 built from source into /opt/nexium-1.5.0
bash build.sh          # fetches and verifies SQLite, then: nx build main.nx --mode fast --cpu x86-64-v3
SQLITE_PATH=/path/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

Nothing prebuilt is downloaded. `install.sh` fetches Nexium's source at one commit, by its full hash, so git checks every byte against the pin. It then bootstraps the compiler with gcc alone: a C seed in the repository builds a first compiler, that one builds the compiler's own Nexium source, and the result must rebuild itself to the same C or the install fails. This takes two to three minutes and about 600 MB of memory. `nx` makes no network request when it builds the server.

## Layout

```
src/main.nx          startup, configuration, the loop's pass order
src/loop.nx          completions, request dispatch, the ready list, idle wait, the nap controller
src/net.nx           rings and connections, the response arena, sends, shutdown and close, request stash
src/uring.nx         io_uring through syscall(): setup and its fallbacks, SQEs, the buffer ring
src/app.nx           the five endpoints, the write queue and the group commit
src/http.nx          request parsing, ids, UTF-8 validation, status lines
src/json.nx          JSON escaping, integer formatting, a strict JSON reader
src/jwt.nx           base64url and HS256 verification
src/sha256.nx        SHA-256 and HMAC
src/db.nx            SQLite: B-tree reads and writes, the SQL fallbacks, the checkpoint thread
src/records.nx       SQLite's record format: varints, serial types, encode and decode
src/sqlite_shim.c    the amalgamation, and one-call wrappers for its private B-tree functions
src/ring_atomic.h    a 32-bit atomic AND and a fence
```
