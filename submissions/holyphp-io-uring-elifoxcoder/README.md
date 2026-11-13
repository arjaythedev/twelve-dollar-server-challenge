# HolyPHP feed API (twelve-dollar-server-challenge submission)

**Language:** HolyPHP 1.0.2, compiled to C11 by the compiler vendored in `toolchain/`.

**Serving mode: direct** — the server listens on `HOST:PORT` itself (the box sets
`0.0.0.0:80`), with **no Nginx in the request path**. If `:80` is already taken
(for example by Nginx), it falls back to `127.0.0.1:3000` so the service still
comes up behind the proxy. Measured on one vCPU, direct mode is ~50 % faster
than the same server behind Nginx, because the proxy costs about as much CPU as
the app itself.

## What's in here

- `server.hphp` — the entire server: HTTP/1.1 keep-alive loop, request parsing and
  routing, HS256 JWT verification on every request, strict JSON validation for
  POST bodies, all SQL, and the write policy (group commit).
- `toolchain/` — the HolyPHP compiler, vendored. Built from source by `build.sh`.
- `install.sh` / `build.sh` / `start.sh` — standard challenge scripts.

## Server-side optimizations

- **The feed and a single post are read from SQLite's B-tree cursors.** `/feed`
  reads the newest twenty `(created_at, id)` keys into an integer vector, walks
  `likes_post_id_idx` once for a dense like-count histogram, then seeks each
  post and author and escapes their payload bytes straight into the response
  buffer. `/posts/:id` uses the same cursors and an index range scan for its
  count. Records are decoded in place without row objects or per-field strings.
  The feed's `posts` table record is decoded inline in typed HolyPHP, matching
  the fixed `schema.sql` layout; invalid or overflow records fall back to the
  generic B-tree decoder. Set `HPHP_FEED_RAW=0` to use the generic decoder for
  every row.
  This is a generic driver binding (`bt_root` / `bt_open` / `bt_last` / …),
  not challenge logic, and it falls back to the SQL path when the pinned
  SQLite could not be built (the binding is a weak reference).
- **A string builder with a JSON escaper.** `sb_new` / `sb_put` / `sb_int` /
  `sb_json` / `sb_take` append into one buffer; the escaper copies runs of
  ordinary bytes with one `memcpy` and uses a 256-entry table per byte.
- **Group commit.** Writes collected during one poll cycle are applied in a single
  `BEGIN IMMEDIATE … COMMIT`; responses are only sent after `COMMIT` succeeds
  (all flipped to 500 on rollback).
- **WAL + `synchronous=NORMAL`**, `temp_store=MEMORY`, 64 MB page cache and
  prepared statements. SQLite mmap is disabled by default; set `HPHP_MMAP=1`
  to enable the tested 256 MB mmap path.
- **One write for small bodies**, header + body written separately for the feed
  (avoids one multi-KB copy).
- **`Content-Encoding: identity`** — compression is optional per SPEC.md, and the
  box is CPU-bound, so the body goes out as-is.
- **TCP_NODELAY**; idle connections held 90 s (spec asks ≥ 65 s).
- **Rate-limited idle sweep.** Idle connections are detected by a cursor that
  advances at most one 2048-entry slice per second (not per loop cycle): at the
  benchmark's spread-out request rate the loop runs ~10k cycles/s, so a per-cycle
  slice walked ~20M entries/s for nothing. A full pass still finishes far inside
  the 90 s window.
- **Connection shedding** just below the usable descriptor table: 61,439 real
  fds in the classic path (the box sets `LimitNOFILE=65535`), or
  `uring_direct_slots() - 1024` with ring-owned descriptors. Shed responses
  carry `Connection: close`, so new users are never refused.
- A strict RFC 8259 parser for POST bodies (the runtime's `json_decode` is lenient).

## Runtime/compiler work (generic, no challenge logic)

The server is plain HolyPHP, but the language implementation was made fast and
allocation-stable for a long-running server. All of this is generic:

- **epoll readiness.** `epoll_add` / `epoll_del` / `epoll_wait_into` let the loop
  wait on thousands of client sockets in O(1); `stream_ready` (poll) is O(n).
- **O(1) socket state.** The runtime used a linear scan over every open socket on
  each `recv`/`send`; it is now indexed by descriptor.
- **Interned string literals.** Codegen emits `hp_str_intern("...")`; the runtime
  caches one `hstr` per literal address instead of allocating a copy per use.
- **Request-scope arena.** `scope_begin()` / `scope_end()` bracket a poll cycle;
  every string allocated in between is released in one shot. `str_persist()`
  copies a value out, `str_free()` / `arr_free()` / `arr_clear()` release heap
  values early. Without this the server leaked ~7.5 KB per request (3 GB in 20 s).
- **No redundant clone for array literals** (`$r = [];` used to allocate three
  arrays).
- **`stream_ready_into`** reuses the caller's array and the pollfd buffer.
- A plain SQLite binding (`sqlite_open`/`prepare`/`bind_*`/`step`/`column_*`/…),
  `stream_set_nodelay`, a JSON string-encoder fix, and `signal(SIGPIPE, SIG_IGN)`.
- **B-tree cursor binding.** `toolchain/src/hp_btree.c` is appended to the pinned
  amalgamation by `build.sh` (it needs SQLite's internals) and exports
  `hp_bt_*_impl`: root page by table/index name, cursor open with the index's
  `KeyInfo`, `first`/`last`/`next`/`prev`/`seek`, rowid and payload. The runtime
  declares those as weak references, so a build against the system library still
  links and the server uses its SQL path.
- **Pointer arithmetic and typed dereference** in the compiler (`ptr + int`,
  `*u8` reads one byte), and a cache key that includes `HPHP_SQLITE_LIB` (a
  cached binary must not silently switch SQLite builds).
- **io_uring backend behind the network primitives.** When the kernel can
  create a ring, one `io_uring_enter` per poll cycle replaces `epoll_wait` plus
  the per-request `recv`/`send` pair: each connection rides one armed
  `IORING_OP_RECV` with a recycled buffer, listeners ride `IORING_OP_POLL`,
  responses flush from owned chunks on the next cycle, and per-fd epochs drop
  completions that arrive after a close. Ring-full pushes park their fd in a
  retry ring; a queued send defers the close until the kernel has taken the
  bytes (the `Connection: close` shed path). Because the kernel caps the SQ
  ring at 32768 entries, the server sizes its keep-alive table to the ring
  (`uring_active()`): above ~28k live connections the surplus users reconnect
  per request instead of waiting for an armed-recv slot. If the kernel
  refuses the ring, every primitive falls back to the plain epoll path;
  `HPHP_URING=0` forces it.
- **Ring-owned descriptors, multishot recv, several rings.** With
  `HPHP_DIRECT=1` (default in `start.sh`), each ring registers a sparse
  65535-slot file table (`IORING_REGISTER_FILES2` +
  `IORING_RSRC_REGISTER_SPARSE`, clamped to `RLIMIT_NOFILE`) and accepts
  straight into it from eight `SO_REUSEPORT` listeners. Their independent
  kernel accept queues spread connection bursts; the generic runtime keeps an
  accept armed on the listener that completed and fills a bounded queue of 512
  single-shot accepts per active ring. If reuseport is unavailable, the server
  falls back to one ordinary listener. Connections then use multishot recv over
  a provided-buffer ring (one SQE for the connection's lifetime). By default,
  receive data is copied into per-connection state and the provided buffer is
  returned immediately. With opt-in `HPHP_RECV_ZEROCOPY=1`, a complete request
  that fits in one selected buffer is parsed in place; partial or multi-buffer
  requests are promoted to an owned string before the buffer is returned. Both
  modes use fixed-file sends and ring CLOSE.
  Direct handles live above every real fd, so the 65535 fd limit no longer caps
  held connections. `HPHP_RINGS`
  (default 6) creates six rings, each with its own sparse table and buffer
  ring: ~393k direct slots at the default. A registered eventfd per ring keeps CQ wakeups
  reliable with `DEFER_TASKRUN`; wait/retry/close paths are ring-aware, and a
  full ring disables only its own accept. Locally: 42/42 with five rings,
  80,000 concurrent connections from eight parallel clients with
  `arm_fail=0`/`parked=0`, and 42/42 on the classic real-fd path.
  `HPHP_DIRECT=0` forces that path; a failed registration falls back
  automatically.

There is no challenge-specific C code: the backend is generic runtime code the
same way epoll and the SQLite binding are.

## Requirements

- `build-essential`, `curl`, `unzip`, `libsqlite3-dev`.
- `build.sh` builds the pinned SQLite amalgamation (3.46.1 by default,
  sha256-checked) with
  `-DSQLITE_THREADSAFE=2` with memory-status tracking disabled and SQLite C APIs
  that the runtime does not expose omitted. Both request and checkpoint
  connections use NOMUTEX and are owned by separate threads; `sqlite3_initialize`
  is called explicitly. SQL syntax and default pragmas are unchanged. Set
  `HPHP_SQLITE_DEFINES` to replace the default define list or
  `HPHP_SQLITE_THREADSAFE` to choose another thread mode. If the download or
  build fails, it falls back to the system library, so an offline box still
  produces a working server. JSON1 and `INSERT … RETURNING` need SQLite ≥ 3.38.

## Run

```bash
bash install.sh   # as root, once
bash build.sh     # as the build user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./start.sh
```

For an optional PGO build, set `HPHP_PGO_DIR` to a GCC profile directory
collected from a representative run of this server. `build.sh` then uses
`-fprofile-use` for the generated runtime; unset it for the normal native
build. PGO data should be refreshed after substantial source changes.

## Measurements

**On the challenge box** (benchmark agent, direct on :80, 5-minute hold):

| commit | users held 5 min | req/s | app CPU | µs/req | errors |
|---|---:|---:|---:|---:|---:|
| `96aa4f5` (B-tree feed) | 110,000 | 10,637 | 83 % | 73 | 0 |
| `8ee9c5a` (send-path fix) | 130,000 | 12,618 | 93 % | 69 | 0 |
| `7a3f61e` (sweep rate-limit, `str_to_int`) | 135,000 | 13,117 | 92 % | 65 | 0 |
| `1569472` (io_uring backend + ring-scale fixes) | **145,000** | 14,093 | 91 % | 60 | 0 |
| `d75dd29` (allocation-free request path) | box run requested | — | — | — | — |

### The allocation round (`2f8507b` … `d75dd29`)

Instrumentation first: `runtime_stats()` exposes generic runtime counters
(ring enters, SQEs/CQEs per op kind, send copies, hstr allocations, poll
cycles); `server.hphp` mirrors them at `/stats` when the operator sets
`HPHP_STATS=1` — bench requests pay nothing for it (the run scripts never set
the env).

Then three measured fixes on the request path, each one attributing its cost
with the counters (or gdb sampling under load) before touching it:

| counter per `/health` request | before | after |
|---|---:|---:|
| hstr allocations (`str_new`) | 7.01 | **0.02** |
| send-payload copies (`send_chunk`) | 1.00 | **0.00** |
| ring `io_uring_enter` | 1.11 | **0.97** |

- `c10afad`: `strpos` scanned by *copying the tail into a throwaway string*
  first (gdb sampling: 3 of 5 allocations per request); it now scans in place
  from the offset. The hot `/health` response is built straight into one
  buffer and handed to the ring send chunk (`stream_send_sb(self-fd, sb)` —
  the builder buffer *becomes* the send chunk, zero payload copies; generics:
  `int_digits`, the SB flow). The parse loop skips the pipelining-`substr`
  for single-request packets.
- `d75dd29`: the recv handout is a pointer, not a copy — with the ring active
  each connection owns one long-lived hstr block the kernel writes into;
  `stream_recv` returns it directly (`HP_F_SCOPED` so retain paths copy and
  close frees it; a caller asking for fewer bytes than arrived still gets the
  classic copy).

Local (WSL microbenchmarks — my machine, not box numbers): a minimal C epoll
server on the same stack costs 54.1 µs/req; this build is at 57.4 µs/req.
Attribution of the remaining gap, measured by removing one piece at a time:
SQLite health check 0.7 µs, response builder 0.2 µs, the rest is the
interpreter/parse and the ring's two completions per request. `/health` is at
0 strings and 0 payload copies per request; p95 1.11 ms → 691 µs, median
436 µs → 359 µs. The box run for `d75dd29` (or later) is requested upstream
with `HPHP_STATS=1` so the same counter split can be read from the $12 box.

#### Leak fixes found by the counters (`06c6b17`)

`runtime_stats()` now also reports `live_allocs`, which turned two silent
leaks into measurements:

- `stream_send_sb` consumed the builder's payload buffer but never freed the
  builder handle: **~55 B leaked per `/health` request** (+14.8 MB RSS over
  270k requests). The handle is freed now; the buffer still becomes the ring
  send chunk (zero copies unchanged).
- the io_uring EOF path set the ring's own EOF flag but not the app-side
  socket record, and `stream_eof()` consults the latter — a client disconnect
  was never noticed. Measured: 21 closed clients left 21 fds open and the
  close counter at 0. The EOF completion now mirrors into the socket record:
  fds return to baseline and `close` equals `accept`.

After both: RSS flat at 14.6 MB over 810k requests (three 30 s windows at
9k rps), `live_allocs` flat, `str_new` 0.02/req, `send_chunk` 0.00/req,
`enter` 0.95/req, 42/42 tests green.

#### Shed-path crash and stall found by ASAN + load (`524fa14`)

The shed regime (above the ring's 28k connection cap) is the churn regime:
every response says `Connection: close`, so the server accepts and closes a
connection per request. Two runtime bugs only showed up there:

- **heap corruption**: `unset()` on an empty map computed `len - 1` and
  touched one element before `keys[]`. The index could hold a duplicate
  entry (inserted twice when a rebuild ran during the add) that survived a
  delete and pointed past the live range. Fixed, plus index maintenance in
  `array_pop`/`array_shift`.
- **infinite loop**: the open-addressing probe loops had no termination
  condition. Once churn left the table without a single empty slot, a lookup
  spun at 100 % CPU and the server stopped answering (measured: 137 of
  26,855 requests, then dead). Probe loops are bounded now and deletes count
  their tombstones against the rebuild budget.
- **the enter timeout never applied**: `IORING_ENTER_EXT_ARG` expects a
  `struct io_uring_getevents_arg`, not a bare `timespec`; the kernel answered
  EINVAL and the fallback ran an unbounded enter (blocks forever when nothing
  is pending). Fixed, with a poll-based fallback for old kernels.

Also: the zero-copy recv re-arm is deferred to the next wait cycle so the
kernel cannot overwrite the buffer while the handler parses it.

Evidence: ASAN build under churn (3 × 20 s at 3000 req/s, shed threshold 32):
before, 3/3 runs died; after, 3/3 alive with zero ASAN reports. 9k rps
keep-alive: 26,855 requests, 0 % errors, server healthy after the run.
42/42 tests green.

#### This round: 60k connections, native auth, one-buffer feed

- **28,000 → 60,000 held connections.** The kernel caps the io_uring SQ ring
  at 32768 entries and the server parks one armed recv per connection, so it
  used to shed everything above 28k; those users paid a TCP handshake plus
  accept/close for every request. Connections beyond the ring cap now
  register in an epoll set (the ring still carries the sends), so the server
  holds up to the descriptor limit. Verified with `HPHP_RING_MAX=10` and 1500
  connections: 45,000 requests, 0 % errors, and the ring counters show only
  the 10 ring connections produced recv completions.
- **native HS256 `jwt_verify`**: split, alg, constant-time signature compare
  and exp in C. The PHP version allocated ~20 strings per authenticated
  request; the like path now allocates 13 per request, down from 29.
- **in-place header parsing** (`str_pos_ci`, `str_int_range`,
  `str_trim_range`): no `strtolower($head)`, no header copy, no
  Content-Length substring.
- **SQLite record decoder** (`bt_cols`): the feed decodes a row's header once
  in C instead of walking varints byte by byte in the application loop.
- **feed response from one buffer**: header prepended into the builder and the
  buffer becomes the ring send chunk (no body copy, no header concatenations).
- **1 KB recv buffer** (was 4 KB): 60 MB instead of 240 MB at 60k connections.
  A controlled alternating A/B on one binary measured 77.3 vs 74.6 µs/req
  (about 3 %) -- a noisy back-to-back comparison had suggested much more, so
  the number here is the conservative one.

Local numbers (WSL, my machine, not box numbers): like 151 → 136 µs cold and
61.5 µs at 4k rps; the user-model mix 76.1 → 74.9 µs at 200 VUs. The box run
is what counts.

#### The wakeup round (`abf7a6d`): one `io_uring_enter` serves many requests

Measured on one machine against the C submission (`c-iouring-public`, #2),
same seed, same user model, alternating runs:

| | C submission | HolyPHP before | HolyPHP after |
|---|---:|---:|---:|
| `/health` µs/req | 40.8 / 39.8 | 98.8 / 100.8 | **40.5 / 41.0** |
| user-model mix µs/req | 40.5 / 40.3 | 129.6 / 194.5 | **47.6 / 45.3** |
| `io_uring_enter` per request | — | 1.63 | **0.14** |

The gap was not the generated code: the C submission's loop waits for up to
128 completions or 2 ms while it has work, so one wakeup serves a batch of
requests. Ours waited for exactly one completion per cycle. `hp_uring_wait`
now does the same (64 completions / 2 ms while busy, block for the first
completion when idle), and `hp_uring_enter` takes microseconds.

That took the trivial path to parity with C and the mix from 1.8× to
1.12–1.18×. The batching costs latency: median 2.5 ms, p95 4.6 ms, p99
6.3 ms locally — far inside the box's p95 < 500 ms.

#### The box said no to the epoll-hold round: back to park-and-retry

The box scored the branch at `56b5187` at **100,000 users** — below the
merged 145,000, and with CPU per request up from 60 to 76 µs. That branch's
connection-cap round had replaced the proven overflow behavior: connections
beyond the ring cap had been parked in the retry ring and re-armed after the
next drain (`1569472`, which scored 145k at 60 µs/req); the hold round routed
them into an epoll set instead. Its first box-scale run carried up to ~32k
epoll-held connections and those requests paid a recv syscall plus epoll
add/del per request. Local tests at 2500 connections pinned to one core
showed the epoll-hold path at 57 µs/req vs 31 µs/req for arm-retry, and the
box confirmed the direction: 76 µs/req where park-and-retry scored 60.

The runtime is back on park-and-retry by default (`HPHP_OVERFLOW=1` opts
into the epoll set for measurement), and the server sheds again at the
ring's armed-recv capacity (28000 under uring), which is exactly the regime
the merged 145k run used: keep-alive up to the ring cap, accept/close churn
beyond it. Batched wakeups, the taskrun flags, the native auth/feed work and
the 1 KB buffers stay.

Rest of the story continues below.





The 64,000 at `c935dfe` was a bug (EAGAIN treated as fatal in the non-blocking
send path), fixed in `8ee9c5a`. `30d155b` rate-limits the idle sweep to one
slice per second. `1569472` puts the network primitives on io_uring and fixes
the scale-time paths (deferred close for shed responses, bounded retry drain,
keep-alive table sized to the ring, `accept4`, listener `TCP_NODELAY`); the
ring is worth about 5 µs/req. Above 145k the search fails on CPU: 150k ran at
93 % CPU with p95 567 ms. The numbers above are what count; the local figures
below are a proxy.

On a VPS shaped like the box (1 vCPU, 2 GB), fresh seed, `wrk`, measuring the
server's own CPU from `/proc`:

| Endpoint | before this round | now |
|---|---:|---:|
| `GET /feed` (5.1 KB seed) | 32.1 µs | **17.9 µs** |
| `GET /posts/:id` | 13.8 µs | **6.4 µs** |
| `POST /posts/:id/like` | 83 µs | **54 µs** |
| `POST /posts` | 83 µs | **62 µs** |

The local proxy reads ~3.2× faster than the box, so it is useful for comparing
builds, not for predicting the score. The steady-state feed is ~3.5 KB (the
twenty newest posts are the ones the load test created), smaller than the 5.1 KB
golden seed.

`bash test/test.sh` → `passed=42 failed=0`, also on the box and in a 1 vCPU /
2 GB KVM VM (Ubuntu 24.04).

#### Native LTO and receive-buffer sizing (local WSL measurements, 2026-11-02)

The build now defaults to native `-O3 -march=native -flto` for the generated
server and bundled SQLite amalgamation; set `HPHP_OPT` explicitly to override
the flags (for example, for a portable or diagnostic build). On a valid-feed
800-VU mix, alternating the previous O2 binary and this build produced
38.82/38.47 versus 37.77/38.01 CPU µs/request (means 38.645 versus 37.890,
about 2.0% lower CPU/request). Each run completed without k6 failures or
receive recovery. These are WSL comparisons, not box results and not a claim
of reaching the user target.

The default provided-buffer ring size is 4096 per ring. An 80k connection ramp
completed with 79,999 successful responses and no receive-recovery events at
4096; 2048 caused 15,866 bounded recoveries, while 1024 caused 43,937. A
thread-local pool of reusable receive scratch buffers reduced WSL RSS from
about 52.5 MiB to 34.5 MiB in a 3×6k connection hold test, with effectively
unchanged CPU/request in the separate mix A/B. This pool retains buffer
capacity only; request contents are returned after parsing and are never
reused as application data.

#### Feed temporary-array lifetime fix (local WSL measurements, 2026-11-02)

`on_feed_bt` created five to six `iarr_new` arrays for each feed and never
released them. The runtime now has a generic `iarr_release` builtin and a
bounded thread-local pool for small integer-array capacities. Release returns
the array storage to the pool; acquisition clears every element, so no
request values survive for reuse. Arrays too large for the pool are freed.
Existing PHP syntax and builtins are unchanged.

Direct (five rings) and classic test suites both pass 42/42, and the 20-row
feed output is byte-for-byte equal to the reference. In an alternating
800-VU WSL mix at about 168k requests/run, the no-pool version rose from 162
to 943,333 live allocations and 205,472 KiB RSS; the pool version rose from
162 to 16,696 and used 47,572 KiB. CPU/request over two alternating pairs was
46.15/45.99 µs without the pool and 43.90/43.85 µs with it (means 46.07 vs.
43.875, about 4.8% lower). A separate 60-second WSL soak completed 1.405M
requests at 21,584 req/s with 0% failures, p95 10.4 ms, p99 19.8 ms, and no
ring arm failures, parked SQEs, or receive recovery. These are local results,
not a box score or a claim of reaching the user target. The same build also
survived 20 repeated 12-second, 800-VU direct-mode stress runs without a crash.

SQLite 3.54.1 was also built from the official amalgamation and tested against
3.46.1 using alternating WSL runs. Both versions pass 42/42 in direct and
classic mode with identical feed output. In the latest four-run A/B, mean
CPU/request was 46.25 us on 3.54.1 and 46.19 us on 3.46.1, so the newer
version showed no measurable gain and the default remains 3.46.1. A separate
four-run mmap A/B measured 46.54 us with mmap disabled and 47.39 us with the
256 MB mapping enabled (about 1.8% lower CPU/request). These are WSL results,
not box scores. Callgrind places much of the feed work in B-tree seeks and
record decoding, range counting, and JSON escaping; those remain the next
optimization targets.

#### Direct-5 bounded ring-0 wait tuning (local WSL, 2026-11-03)

On the same binary, with the server pinned to one CPU and the exact seed/mix
at 800 VUs for 20 s, disabling the bounded ring-0 wait measured 37.6/37.1 us
server CPU per request. Enabling it at the 1 ms ceiling measured 18.2/18.7 us.
Raising the ceiling to 4 ms measured 14.3/14.3 us; the 2 ms control measured
16.0/16.4 us. All runs had 0% HTTP failures. The 4 ms runs had p95 137–139 ms and p99 165–167 ms.
The later exact 100k hold favored 64 CQEs / 2 ms: p95 8.9–9.0 ms and p99 11.9–12.1 ms, versus about 42 ms / 210 ms for 256 / 4 ms. `start.sh` now selects 64 / 2 ms for multi-ring Direct mode; `HPHP_BATCH_N` and `HPHP_BATCH_US` can override them, and `HPHP_RING0_WAIT=0` keeps the prior path.

A 60 s run at 1,600 VUs with the 4 ms setting completed 551,435 requests at
15.8 us CPU/request, 0% failures, p95 265.8 ms and p99 346.6 ms. These results
show a large same-host CPU reduction and a stable local soak, but they are WSL
measurements: they do not establish a box score or 300k simultaneous live
connections. That earlier extrapolation was superseded by exact challenge-hold
runs: 160k passed the local latency gates only with the background checkpoint
worker; 200k failed them badly. These are WSL results, not box scores.

#### B-tree batched column decode experiment (local WSL, 2026-11-03)

The generic `bt_read` runtime builtin was enabled experimentally for feed and
single-post column extraction, then compared against the existing typed
`bt_col_*` path in an A/B/B/A sequence on the same binary (800 VUs, 30 s each).
The runs reported 17.8 and 20.3 us CPU/request with the feature off, and 19.5
and 19.5 us with it on; all runs had zero HTTP failures. The sequence drifted,
so it establishes neither a gain nor a reliable regression. The candidate's
per-request integer-array allocation was removed; the existing typed path
remains in use. These are WSL microbenchmarks, not box results.

#### Typed response-builder pool experiment (local WSL, 2026-11-03)

Generated code uses the typed `sb_new()` fast path, which does not acquire
from the reusable response-buffer pool used by the hval ABI. Enabling pool
acquisition for the typed path and measuring the same binary in an A/B/B/A
800-VU, 30-second mix yielded 18.2/19.1/19.3/18.7 us CPU/request and
3.60/3.74/3.72/3.63 ms p95; all runs had zero HTTP failures. The candidate was
slower in both pairs, likely because the shared pool can supply a small buffer
that a 12 KB feed builder immediately has to grow. The change was reverted.
These are WSL measurements, not box results.

#### B-tree range-count app-path experiment (local WSL, 2026-11-03)

The existing generic `bt_count_range` builtin was wired into an optional pure
HolyPHP feed path to replace per-post like-cursor iteration. It produced the
exact golden feed and passed all 42 tests in Classic and Direct-5. On the same
binary at 800 VUs for 30 seconds in A/B/B/A order, CPU/request was
18.0/23.5/26.5/20.8 us, with zero HTTP failures. The drift prevents a clean
causal estimate, and both candidate runs were slower; the app path was removed.
This partial port adds a post pre-scan and arrays but does not include the
leader's generic range scan over post rows. It is not a performance result or
a box score.

At 280k configured users, a current-build WSL hold accepted all 280,002
connections but failed latency gates (p95 2.82–2.85 s, p99 4.19–4.23 s). The
same-host C leader also failed (p95 about 1.65 s, p99 about 5.0 s), so this
overloaded co-located harness cannot determine target-box capacity.

#### Generic B-tree batched-seek prefetch test (local WSL, 2026-11-03)

Copied the C leader's page-boundary prefetch into the generic multi-rowid
cursor path behind `HPHP_BT_MULTI_PREFETCH=1`, with the same optional
batched-author HolyPHP feed path enabled in every run. Classic and Direct-5
functional suites both passed 42/42. The 800-VU, 30-second A/B/B/A run measured
21.6/23.5/25.3/22.3 us CPU/request and p95 4.48/4.87/5.43/4.54 ms; every run
had zero HTTP failures. The candidate did not improve the results and the
sequence drifted, so the prefetch change was removed. This WSL test does not
justify a speed claim or box estimate.

#### Listener-group accept rearm (local WSL, 2026-11-04)

The direct runtime now rearms an accept on the same `SO_REUSEPORT` listener
after each completion. Without that, the bounded ring-wide accept pool could
temporarily leave a busy listener with no outstanding accept while the ring
waited on other listeners. The challenge suite passed 42/42 in both Direct
and Classic modes. A staged synthetic run then held 280,000 concurrent
connections for over five minutes; all 280,000 `/health` requests returned
HTTP 200, and all connections later closed cleanly. Five direct rings were
active, with no parked SQEs or accept-arm failures. WSL's original
`nf_conntrack_max` of 262,144 blocked the fourth 70,000-connection batch, so
the local limit was temporarily raised to 1,048,576 for this run and restored
afterward. This is a local idle-connection capacity and stability check, not
the challenge's box benchmark or a throughput result.

#### Typed HolyPHP feed record decoder A/B (WSL, 2026-11-07)

The fixed `posts` SQLite record can be decoded inline in typed HolyPHP, matching the top submission's single-pass record decoder. `start.sh` enables this path; `HPHP_FEED_RAW=0` retains the generic `bt_cols` decoder. The inline path checks header and payload bounds and falls back to the generic decoder if a record does not fit.

On the real seed database, same-binary A/B/B/A at 800 VUs (30% likes, 5% posts; 5 s ramp + 15 s active) had zero HTTP failures. Generic decode averaged 17,523 req/s, p95 41.64 ms, p99 57.43 ms, and 16.72 µs process CPU/request. Inline decode averaged 17,824 req/s, p95 39.73 ms, p99 52.16 ms, and 16.48 µs CPU/request. These are WSL diagnostics, not box results; run-to-run warm-up drift remains visible.

#### Cache, warm-up, and author-batch experiments (WSL, 2026-11-12)

These are local diagnostics, not challenge scores or user-count estimates.
`bench/sqlite-cache-mix.js` preserves the endpoint order and 15% like / 2%
post-write probabilities in `bench/load.js`, but removes think time to expose
request-service cost. All runs used the real seed database, one server CPU,
Direct-6, the same binary (`3333e4f…`), GCC 13.3, `-O3 -march=native -flto`,
and bundled SQLite 3.46.1. k6 ran on separate CPUs; its CPU consumption was
recorded to catch generator saturation. The earlier cache/warm numbers in
this subsection were collected with only three client CPUs and are superseded:
the generator was CPU-saturated, so those measurements are invalid.

With 800 VUs for 15 s in A/B/B/A order, a 64 MiB cache averaged 21.91 µs
server CPU/request; 256 MiB averaged 21.90 µs. There were zero HTTP errors.
The SQLite status showed roughly 5.4 million cache hits, 287 misses, no spills,
and about 5.2 MiB cache use after the 64 MiB run. The larger cache did not
change CPU/request meaningfully. Warming the pages at startup (64 MiB fixed)
averaged 21.82 µs/request versus 22.07 µs without warm-up, a 1.1% difference
below the promotion threshold. Warm-up increased startup from about 150 to
379 ms and RSS from about 46.5 to 85.5 MiB; p99 did not improve consistently.
Keep the defaults at 64 MiB and warm-up off.

The measured feed hotspot was resolving the 20 authors after collecting feed
posts. The generic `bt_multi_seek` implementation is already enabled through
`HPHP_FEED_BATCH_USERS=1`; turning it off restores one `bt_seek` per author.
At 1,000 VUs for 30 s in A/B/B/A order, batched lookup averaged 22.36 µs
CPU/request versus 23.76 µs with individual seeks (5.9% lower). Both paired
comparisons favored batching by 4.1% and 7.8%. Mean request rate rose from
39.8k/s to 42.4k/s, mean p95 fell 4.2%, mean p99 fell 4.1%, and all runs had
zero HTTP failures. The server remained at one CPU; k6 used about 9.6 of its
15 allocated CPUs. Direct and Classic challenge suites each passed 42/42
afterward. This is a repeatable WSL signal for the already-enabled generic
batch path, not evidence that 280k users are achieved on the challenge box.

#### Five versus six rings; 280k async-client attempt (WSL, 2026-11-12)

The 1,000-VU, 30-second no-think-time micro-mix compared five and six rings
in two reversed A/B/B/A sequences. Five rings averaged 24.77 µs CPU/request
and six averaged 24.23 µs across all eight samples (about 2.2% favoring six),
below the promotion threshold. Same-configuration samples varied by 4–6%,
and the reversed sequence did not reproduce a clear advantage. Keep six rings;
five has enough theoretical slots for 280k, but this test does not establish
that reducing the ring count improves request cost.

A separate 280k-user diagnostic used 16 Python asyncio shards, 45 s ramp and
90 s hold. It was invalid as a server-capacity result: at hold time the summed
minimum active client tasks were only 28,529, the server reported 138,534 live
connections, and the client run recorded 94.5% failures with p95 around 93.8 s.
The WSL VM reached its 4 GiB memory cap and used swap while the generator was
creating hundreds of thousands of per-user Python tasks. The temporary local
conntrack limit was restored afterward. Do not interpret these figures as
HolyPHP's 280k capacity or latency; a lower-overhead multi-connection generator
or the challenge box is required for that measurement.

#### Multi-ring wait and answer-batch diagnostics (WSL, 2026-11-12)

Compared the current 64-CQE wait to the Rust leader's 256-CQE wait using the
same binary, 280,000 configured clients, 16 C-generator shards, 45 s ramp,
90 s run, Direct-6, a fresh seed DB, one pinned server CPU, and
`HPHP_DIAG=1`. The answer budget remained 2,048. At 64 CQEs, clients completed
662,738 requests and the server reported 251,909 accepts; at 256 CQEs, clients
completed 798,794 requests (+20.5%) and the server reported 264,708 accepts.
The measured server CPU time was 66.8 s and 64.4 s respectively. Per-shard p95
ranges were 27.290–27.401 s at 64 and 27.499–28.147 s at 256; per-shard p99
ranges were 45.148–45.925 s and 38.938–41.687 s. All shards reported zero
HTTP errors. A reverse-order repeat completed 689,335 requests at 256 versus
657,006 at 64, but p95 worsened from 28.563–28.816 s to 31.844–33.281 s and
p99 worsened from 44.572–46.302 s to 48.498–49.116 s. Server accepts were
239,109 at 256 versus 250,886 at 64. The first throughput gain did not repeat
with acceptable tail latency, so `start.sh` retains 64; the 256-CQE value stays
an explicit experiment. Neither run accepted all 280,000 clients within 90 s.

The 4,096 answer-budget run completed 666,188 client requests and had 10,881
receive-recovery enqueues; 8,192 completed 593,714 requests and had 134,472
enqueues. Both were worse than the default 2,048 run, so keep that budget.
These WSL measurements identify a promising runtime setting; they are not a
challenge-box result or proof of 280k capacity. The large remaining p95 points
to a deeper high-load scheduling or service-cost bottleneck.

#### Busy-aware ring-0 wait (WSL, 2026-11-12)

The Rust loop only performs its bounded ring-0 wait when its read and ready
queues are empty. HolyPHP previously waited for the configured CQE count even
when it had CQEs or deferred ready sockets to process. Added generic
`HPHP_RING0_BUSY` (default `1`) to skip that wait while such work is pending;
set it to `0` to retain the prior behavior.

The candidate and control used the same server binary, Direct-6, 280,000
configured clients (16 C-generator shards), a 45 s ramp, and a 90 s run in
A/B/B/A order. Server requests / accepted connections were: old 608,486 /
240,134; new 670,977 / 257,542; new 636,302 / 249,862; old 686,634 / 256,006.
All runs had zero client HTTP errors. The pair means were nearly flat for
requests (+0.9% for busy-aware) and modestly higher for accepts (+2.3%); the
candidate used about 2.6% more server CPU. Generator 0's p95/p99 were 32.566 /
49.588 s, 29.684 / 44.306 s, 30.355 / 44.502 s, and 27.801 / 48.355 s. These
are one shard's values, not global percentiles; every run remained far outside
the p95/p99 gates. This is a small, noisy scheduling signal, not evidence of
280k capacity. No box score is claimed.

#### Per-ring timing and Rust-style SQ-full flush (WSL, 2026-11-12)

Added `HPHP_DIAG=1` measurements for application read/flush time, per-ring
`io_uring_enter` and submit-only time, CQE drain time/counts, receive-CQE to
application handoff, and response enqueue to SEND completion. The receive-age
buffers are allocated only in diagnostic mode; ordinary response chunks keep
their original size when diagnostics are off. The external load harness also
samples process user/system ticks, RSS/swap, TCP socket memory, WAL size, and
listen-backlog counters.

The high-load trace showed the 250k bottleneck is real: `answerq` grew above
100k, with several seconds of receive-to-application delay in the later rings.
At 280k, the diagnostic run accepted all 280,000 connections by the end of its
90-second generator run, but `answerq` reached 228,889. The final ring live
counts were 64,511 / 64,511 / 64,511 / 64,511 / 21,956 / 0. Per-shard p95/p99
averaged 27.0/41.0 seconds; this was not a stable 280k hold. Linux `ListenDrops`
and `ListenOverflows` each rose by about 265k during the ramp, so connection
setup itself also experienced substantial backlog pressure. This was WSL with
diagnostics enabled, not a box result.

Compared SQ-full `IORING_ENTER_GETEVENTS` flushing (old) against submit-only
flushing (Rust-style) in the same binary, A/B/B/A, Direct-6, 250k configured
clients, 16 C-generator shards, 45-second ramp and 90-second run. Every run
reported 250,000 successful client connections and zero HTTP errors. Request
totals were 793,693 / 931,117 / 926,628 / 857,490. The paired means were
825,592 old versus 928,873 submit-only (+12.5%); server CPU/request was 83.5
versus 79.0 microseconds (-5.4%). Shard-averaged p99 improved about 4.9%, while
p95 was about 3.2% worse on average and varied widely between same-mode runs.
All p95/p99 values remained far beyond the challenge gates, so this is a
throughput signal rather than a capacity pass. `start.sh` now defaults to the
submit-only flush; set `HPHP_SQ_FLUSH_GETEVENTS=1` for the previous behavior.

#### Background checkpoint: restart the WAL (WSL, 2026-11-13)

Under sustained write load the background PASSIVE checkpoint was almost always
overtaken by new commits. It reported completion, but SQLite could never
restart the WAL from frame 0, so the WAL grew until the 65,536-frame forced
in-loop checkpoint ran on the event loop. The runtime now checks whether the
WAL generation changed during the checkpoint. If it did not, the next commit
restarts the log. If it did, writes are held briefly while the remaining tail
is copied, the same approach the current leader uses. `HPHP_CKPT_RESTART=0`
restores the previous behaviour. The runtime also drains each ring's eventfd
with a single `read()` only when that ring produced completions, and string
builders reuse capacity from the existing response-buffer pool (capacity only,
never contents). All changes are generic runtime code; `server.hphp` is
unchanged.

Same-binary A/B/B/A in a Box-like harness (1 pinned server CPU, 1,800 MiB
cgroup without swap, tcp_mem at twice the Box values, 120,000 connections,
60 s ramp + 4 min hold, 16 load generators on separate CPUs):

| | restart off | restart on |
|---|---|---|
| Peak WAL size | 211–260 MB | 18–19 MB |
| Peak page cache | 254–299 MB | 54 MB |
| Disk writes | 1.5–2.5 GB | 1.05 GB |
| Checkpoints | ~2,100 | ~90 |
| CPU per request | 70.96 / 70.81 µs | 70.24 / 70.00 µs |

`PRAGMA integrity_check` returned `ok`. Direct and Classic suites each passed
42/42. These are WSL diagnostics, not Box results. Above about 244k
connections the WSL host is limited by Hyper-V interrupt overhead, and even
the Rust leader fails there, so local runs cannot confirm 280k. The Box
pattern so far was 257k/270k passing the 2-minute probe but failing the
5-minute hold, and 280k failing at 76% CPU. That pattern is consistent with
WAL growth followed by a blocking checkpoint, but it is not proven. A Box run
is required.

#### SQLite file locking and WAL write calls (WSL, 2026-11-13)

Both switches below are **off by default** now. The box run at `f1f8d45`, with
both on, held 230,000 users (30 µs/req, 653 MB peak). 240,000 failed on
p95/p99 at 77 % CPU. That is below the default VFS's 250,000, so `start.sh`
leaves SQLite's default VFS in place and the next run measures the runtime
changes alone. Set `HPHP_SQLITE_VFS=unix-excl` and
`HPHP_SQLITE_WAL_COALESCE=1` to use them.

With `HPHP_SQLITE_VFS=unix-excl`, SQLite opens through the `unix-excl` VFS. The process holds the
database lock, so transactions take no `fcntl()` locks, and the WAL index
lives in heap memory shared by the request and checkpoint connections. The
checkpoint connection uses the same VFS as the request connection. A thin
wrapper over that VFS (`HPHP_SQLITE_WAL_COALESCE=1`) buffers a commit's
sequential WAL frame writes and writes them with one call when the commit
frame arrives. This happens inside SQLite's own write call, before the WAL
index is updated and before `COMMIT` returns. Reads, syncs, truncates,
unlocks and file controls flush first, so SQLite sees the same bytes in the
same order. `HPHP_SQLITE_VFS=` and `HPHP_SQLITE_WAL_COALESCE=0` (the defaults) keep the
previous behaviour.

The runtime also changed in smaller ways:
- String builders keep headroom in front of the body, so the response header
  is prepended without moving the body.
- The response-buffer pool has separate small and large classes.
- Integer formatting no longer goes through `snprintf`.
- The small builder appends are inline.
- The JWT check uses the CPU's SHA-256 instructions when present
  (`HPHP_SHA_NI=0` disables them), and base64url decoding is table-driven.
All of these are generic runtime changes; `server.hphp` only reads the VFS
name from the environment.

Syscalls per request at 8,000 connections (same load, perf tracepoints):

| | before | after |
|---|---|---|
| `fcntl` | 0.28 | 0 |
| `pwrite64` | 0.41 | 0.085 |
| CPU per request | 42.5–43.6 µs | 38.7 µs |

At 120,000 connections in the Box-like harness the CPU difference is smaller
(68.2 → 67.2 µs per request in the same A/B sequence). Network processing
dominates there.

Checks:
- Direct and Classic suites pass 42/42 with every VFS combination.
- Three `kill -9` rounds under write load, with checkpoints forced every 64
  frames, left `PRAGMA integrity_check` at `ok`.
- No sampled acknowledged post was missing.

These are WSL diagnostics, not Box results.
