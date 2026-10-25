# C + io_uring + SQLite B-tree internals (`c-iouring-public`)

A single-threaded C server that treats SQLite as a library of B-trees: raw io_uring syscalls, a SIMD HTTP parser
generated from the API spec, one multi-buffer HMAC call for a whole batch of JWTs, and reads that walk SQLite's
tables through SQLite's own cursors. Every curse has an off switch (`CURSED=boring`), and the boring build passes
the same 42 checks. Everything is built from source on the box.

## Versions
| Piece | Version |
|---|---|
| Language / compiler | C11, gcc 13 (Ubuntu 24.04 `build-essential`), `-O3 -march=native`, PGO |
| Database | SQLite **3.54.1** amalgamation, downloaded and sha256-pinned by `build.sh`, plus one small patch (below), compiled into the binary |
| HTTP | own parsers: `components/http.h` (regenerated from `gen/spec.json` by `gen/httpgen.py`), `components/bp.h` (lane-parallel batch parsing) and `components/fbparse.h` (SIMD general parser with picohttpparser `465a7ff`'s API and identical validation) as the slow path; others selectable at build time (below) |
| Event loop | own, io_uring via raw syscalls (no liburing). Needs Linux ≥ 6.1; 6.8 is the target |
| JWT | own HS256, AVX-512VL (ymm) 8-lane HMAC-SHA256 on CPUs with AVX-512VL, AVX2 otherwise |

**Direct mode: no Nginx.** Listen on `HOST:PORT` (e.g. `0.0.0.0:80`).

## Build and run
```bash
sudo ./install.sh          # apt: build-essential curl unzip ca-certificates python3
./build.sh                 # SQLite download + patches, parser generation, PGO build -> bin/server (~4 min on 1 vCPU)
./start.sh                 # foreground; reads SQLITE_PATH, JWT_SECRET, HOST, PORT
```
`build.sh` falls back to a plain `-O3` build if any PGO step fails. `PGO=0 ./build.sh` skips PGO. A full PGO build is
two `-O3` passes plus training: about 4 minutes on a 1-vCPU Xeon (one pass measured at 1 min 49 s), about 2 minutes
with `PGO=0`.

### Build options (the defaults are what we submit)
| Variable | Values | Default |
|---|---|---|
| `PARSER_FAST` | `batch` (lane-parallel `bp_parse` for the first request of each connection in wakeups with ≥ 8 connections, `fast_parse` for the rest), `single` (`fast_parse` per request), `learn` (`fast_parse_learn`, runtime-learned header layout), `slow` (no fast path) | `batch` |
| `PARSER_SLOW` | the general parser for anything the fast path declines: `fb` (our `fbparse.h`, picohttpparser's exact results, 16–47% faster), `pico`, `llhttp`, `uws` (C++, HTTP/1.1 only), `hparse` (Zig 0.15, needs `zig`), `httparse` (Rust, needs `rustc`) | `fb` |
| `PGO` | `1` / `0` | `1` |
| `CC` / `CXX` | C / C++ compiler (C++ only for `PARSER_SLOW=uws`) | `gcc` / `g++` |
| `ARCH` | target flags | `-march=native` |
| `WARN` | extra warning flags (`tools/dev.sh` uses `-Wall`); printf format mismatches are always errors | none |
| `REUSE_SQLITE` | `1`: keep an existing SQLite object built from the same sources (development builds) | unset |

### How cursed (build time; `config/cursed.sh`)
Every unusual thing this server does to SQLite can be turned off at build time, one knob at a time, or all at once
with `CURSED=boring` (all knobs are in `config/cursed.sh`), which gives a by-the-book build: stock SQLite linked as
its own unit and used only through its public API, prepared SQL, SQLite's own VFS and autocheckpoint, normal
locking, scalar JWT, picohttpparser for every request.
| Variable | Cursed (default, submitted) | Boring |
|---|---|---|
| `SQLITE_PATCHES` | `pcache1-maxkey` | `none`: stock SQLite 3.54.1 |
| `SQLITE_LINK` | `unity`: `components/engine.c` `#include`s `sqlite3.c`, one unit, so it can use SQLite's internals | `separate`: `sqlite3.c` is its own unit, and `src/engine_public.c` uses only the public API (`sqlite3.h`). The build checks that no other SQLite symbol is referenced. Works with stock or patched SQLite, and needs the `sql` / `stock` / `sqlite` or `inline` / `keep` values below |
| `DB_READS` | `direct`: reads through SQLite's own B-tree cursors | `sql`: the spec's prepared SELECTs |
| `DB_VFS` | `cursed`: WAL write-coalescing and file-size-cache shim | `stock`: SQLite's unix VFS |
| `DB_CKPT` | `bounded`: SQLite's own RESTART checkpoint on the event loop every 4,096 WAL frames, after the batch's replies are submitted; the WAL is reused in place, no write gate, `SQLITE_THREADSAFE=0` (Rust #82's checkpoint style). Or `uring`: the engine runs SQLite's checkpoint step itself, as a state machine on the event loop's io_uring, with a write gate; `thread`: a checkpoint thread on a second connection; `inline`: PASSIVE after the replies. `CURSED=boring` refuses `uring` | `sqlite`: SQLite's autocheckpoint |
| `DB_WAL_DROP` | `keep` with `bounded` (its small WAL stays hot); `drop` with `uring` / `thread`: checkpointed WAL pages are dropped from the page cache, and the WAL is truncated at restart | `keep` |
| `DB_LOCKING` | `exclusive` | `normal` |
| `JWT` | `simd`: multi-buffer batch HMAC | `scalar` |
| `PARSER_FAST` / `PARSER_SLOW` | `batch` / `fb` | `slow` / `pico` |

PGO works the same way in both link modes: every object, including a separately compiled `sqlite3.c`, is
instrumented and trained by the server's own run against a synthetic database. SQLite is trained by the server's
real workload rather than a separate SQLite benchmark.

`CURSED=lawful` sits in between: stock SQLite as its own unit, used only through its public API (prepared SQL,
SQLite's VFS, inline checkpoints), with every curse outside SQLite still on (batch HTTP parsing, `fbparse.h`, SIMD
JWT, exclusive locking). `CURSED=max DB_CKPT=thread` keeps everything else but checkpoints from a thread through
SQLite's own `sqlite3_wal_checkpoint_v2`.

In every mode, writes are prepared SQL `INSERT`s, one transaction covers each batch, and every response waits for
its `COMMIT`. The server prints the mode it was built with at startup.

All 24 parser combinations pass the 42-check suite (Linux arm64, gcc 13; `tools/matrix.sh`). The default uses only
`build-essential`. The optional parsers' sources are in `third_party/parsers/` (llhttp v9.4.3, uWebSockets v20.80.0, hparse
`4fb3d83`, httparse v1.10.1, each with its licence), wrapped behind picohttpparser's `phr_parse_request` signature.

## Runtime knobs
`start.sh` sets none of these, so the scored server runs on the compiled defaults below. The table is generated from
the code by `tools/knobs.py`; the `CURSED_*` and `LOOP_TEST_*` variables exist for experiments.

<!-- runtime-knobs:begin -->
**The challenge's runtime contract**

| Variable | Default | What it does |
|---|---|---|
| `SQLITE_PATH` | required | the database file (a fresh copy of the seed) |
| `JWT_SECRET` | required | HS256 secret for the Bearer tokens |
| `HOST` | 0.0.0.0 | listen address (IPv4) |
| `PORT` | 80 | listen port |

**Event loop and connections**

| Variable | Default | What it does |
|---|---|---|
| `CURSED_GUARD_FREEZE` | 920 | band mode: CPU guard, permille busy: above this, the close fraction stops rising (1000 or more: guard off) |
| `CURSED_GUARD_RELAX` | 850 | band mode: CPU guard, permille busy: below this, the frozen close fraction is released |
| `CURSED_BUSY_SRC` | 0 | band mode: CPU guard's busy signal: 0 auto (1 CPU online: /proc/stat; pinned to one CPU: its cpuN line; cgroup quota <= 1.5: cpu.stat; else getrusage), 1 getrusage, 2 /proc/stat, 3 cgroup quota, 4 the pinned CPU's line |
| `CURSED_DEFER_ACCEPT` | 0 | TCP_DEFER_ACCEPT seconds on the listener (0: off; Linux only) |
| `CURSED_LINGER` | 0 | close style: 0 auto (linger only if the peer may still be sending), 1 always linger, -1 never |
| `CURSED_BATCH` | 128 | completions to wait for per wakeup under load |
| `CURSED_WAIT_US` | 2000 | longest extra wait for that batch (µs) |
| `CURSED_BACKLOG` | 4096 | listen backlog (4096 absorbed 100 ms loop stalls at high reconnect rates without overflows, measured on a 1-vCPU Xeon) |
| `CURSED_RECV_MODE` | 0 | 0 (auto) / 1: armed multishot recv; 2: idle connections hold a multishot poll (less kernel memory per idle connection, but a mass close keeps FIN buffers charged: +631 MB at 300k) |
| `CURSED_EPOLL` | 0 | 1: use the epoll backend even when io_uring is available (it's also the automatic fallback) |
| `CURSED_RX_TS` | 0 (1 when CURSED_DEADLINE_MS is set) | 1: kernel receive timestamps, so queue delay includes time spent in socket buffers |
| `CURSED_DEADLINE_MS` | 0 (off) | deadline shedding: a request whose bytes waited longer than this in the kernel's receive queue gets an immediate 503 (turns on receive timestamps) |
| `CURSED_CONN_POLICY` | 0 | 0: no `Connection: close` shedding (the emergency floors stay on); 1: close a fraction of replies under memory pressure |
| `CURSED_SHED_MODE` | headroom | `headroom`: the close fraction is a function of available memory alone; `band`: the older cap-and-band rule (the cap knobs, band and CPU guard below apply only to it) |
| `CURSED_SHED_HI_MB` | 400 | headroom: no shedding while available memory (~1 s average) is at least this (MB) |
| `CURSED_SHED_LO_MB` | 200 | headroom: the close fraction reaches its maximum at this much available memory (MB) |
| `CURSED_SOCK503_MBPS` | 50 | socket-memory pressure valve: if socket-buffer memory rises more than this (MB) within 1 s while above 64 MB, or exceeds 160 MB, while available memory is under CURSED_SOCK503_AVAIL_MB, queued requests get a fast 503 (keep-alive, no DB work) until it drains below 48 MB (0: off) |
| `CURSED_PRESS_PENDING` | 20000 | with CURSED_DRAIN=1: also raise the pressure valve (same memory gate) once the loop's own backlog exceeds this many requests; it releases below half |
| `CURSED_PRESS_AGE_MS` | 300 | with CURSED_DRAIN=1: ...or once its oldest queued request has waited longer than this (ms); it releases below half |
| `CURSED_DRAIN` | 1 | 1: drain-first receive: requests are copied out of the kernel's socket buffers into compact buffers at once and handled in chunks; 0: the earlier receive path |
| `CURSED_DRAIN_CHUNK` | 0 | drain-first: most requests handed to one batch (0: the loop default, 256) |
| `CURSED_DRAIN_BUDGET` | 0 | drain-first: most completions drained per loop pass (0: the loop default, 8192) |
| `CURSED_SOCK503_AVAIL_MB` | 450 | socket-memory pressure valve: only below this much available memory (MB) |
| `CURSED_SOCK503_BUDGET` | 0.006 | the valve's error budget, a fraction: it answers 503 only while a token bucket allows it. The bucket earns this many tokens per request answered, starts full and holds at most `CURSED_SOCK503_BURST`; when it's empty, requests are served normally. Bounds a run's 503s to burst + budget × requests (0: no cap) |
| `CURSED_SOCK503_BURST` | 20000 | the error budget's bucket size: enough to drain the largest backlog seen in our load tests (~19k requests) |
| `CURSED_SHED_FMAX` | 500 | headroom: the maximum close fraction, permille |
| `CURSED_CONN_CAP` | 0 | band mode: fixed connection cap instead of the memory-derived one (0: derive) |
| `CURSED_RESERVE_MB` | 0 | band mode: memory kept free for everything else in the cap formula (0: the loop's 250 MB) |
| `CURSED_CAP_CEILING` | 380000 | band mode: hard ceiling on open connections, also for a fixed CURSED_CONN_CAP (the Xeon OOMs at ~450k sockets) |
| `CURSED_SHED_ENGAGE` | 950 | band mode: shed band, permille of the cap: start Connection: close shedding at this fill |
| `CURSED_SHED_RELEASE` | 880 | band mode: shed band, permille of the cap: stop below this fill (must be below ENGAGE) |
| `CURSED_MEM_ACCEPT_MB` | 100 | emergency floor: stop accepting below this much available memory (MB) |
| `CURSED_MEM_SHED_MB` | 60 | emergency floor: close long-idle keep-alives below this much available memory (MB) |
| `CURSED_SHED_FIN` | 1 | 1: emergency closes use FIN; 0: RST |
| `CURSED_STATS` | 0 | print loop, app and engine stats every N seconds to stderr (0: off) |

**Database experiments**

| Variable | Default | What it does |
|---|---|---|
| `CURSED_CACHE_KIB` | 8192 | SQLite page cache (KiB) |
| `CURSED_CKPT_FRAMES` | 4096 (bounded, uring, thread) / 16384 (inline) | checkpoint pass size in WAL frames |
| `CURSED_CKPT_THREAD` | 1 in thread mode | thread mode only. 1: checkpoint from the background thread; 0: inline |
| `CURSED_CKPT_RESTART` | 32768 | the write gate's drain mark (WAL frames), from the engine's Xeon sweep |
| `CURSED_CKPT_NICE` | 0 | nice level of the checkpoint thread (lower = drains finish sooner at a pegged CPU, but take more CPU from reads) |
| `CURSED_COALESCE` | 0 | 1: share one feed computation between the feed requests of a batch. Off until the organizer rules on whether that counts as caching (rule 5) |

**Test-only**

| Variable | Default | What it does |
|---|---|---|
| `CURSED_TEST_HOLD_MS` | 0 | pretend writes are paused N ms of every 200 ms (exercises the write hold) |
| `LOOP_TEST_ARM` | unset | kernel-memory measurement hook: `poll` arms a multishot poll instead of a recv, `none` arms nothing (the connection then never reads) |
| `LOOP_TEST_NO_INLINE_RECV` | 0 | 1: disable the same-pass recv after a readiness completion (A/B only) |
<!-- runtime-knobs:end -->

## Code layout
```
install.sh build.sh start.sh   the challenge's three entry points
config/                        build time: compiler flags (sqlite.sh), how cursed (cursed.sh), which HTTP parsers
                               (parsers.sh), and the compile/link recipe (compile.sh)
gen/                           the HTTP parser generator and the API spec it reads
patches/sqlite/                our SQLite patches, applied by build.sh to the pinned amalgamation (one)
src/                           run time, our glue: app.c (the batch pipeline), main.c (process setup), train.c (PGO
                               driver, build time only), config.h (every compile-time choice), unit_*.c (the three
                               translation units)
components/                    our components, each from its own exploration: event loop (loop.c), SQLite engine
                               (engine.c), POST body parser (body.c), JWT (jwt.c), HTTP parsers (http.h, bp.h, fbparse.h)
third_party/parsers/           picohttpparser and the optional slow parsers, with their licences
```
Everything in `src/` follows `.clang-format`. `components/` and `third_party/` are kept byte-identical to their
sources, so they keep their owners' style.

## Design
One process, one thread: the event loop, which also runs the WAL checkpoints. Every loop wakeup is one batch:
1. **Receive.** io_uring rings of 65,535 direct descriptors each (created on demand), 256 single-shot accepts in
   flight. Each connection has an armed multishot recv into a provided-buffer ring. Up to 128 completions or 2 ms per
   wakeup. If io_uring is unavailable, the loop falls back to epoll. With drain-first (on by default), each wakeup
   copies the received bytes out of the kernel's socket buffers into compact per-connection buffers at once and hands
   them to the app in chunks of up to 256, so a busy loop holds its backlog in ~0.5 KB per request instead of a ~4 KB
   kernel buffer.
2. **Parse.** The parser matches the API's request lines from the first bytes and finds the headers it needs with
   SIMD masks. It's generated from the API spec; a traffic profile only orders the templates and enables paths, it
   contributes no bytes. When a wakeup has 8 or more connections, their first requests are parsed lane-parallel.
   Anything unusual (HTTP/1.0, `Connection` headers, odd framing) goes to the general parser `components/fbparse.h`.
3. **Authenticate.** All Bearer tokens of the batch are verified in one multi-buffer HMAC call. Every signature is
   recomputed, and `exp` is required.
4. **Database.** All requests of the batch run in one SQLite transaction (group commit), in arrival order:
   - Reads use SQLite's B-tree cursors directly: the feed's like counts come from one index range scan, and its 20
     author lookups run as one batched seek.
   - Writes are prepared SQL `INSERT`s.
   - A small VFS shim writes each commit's WAL frames with one `pwrite` and caches the DB file size while the lock
     is held.
   - Checkpoints run on the event loop: every 4,096 WAL frames (~16 MB), once the batch's replies have been handed
     to the kernel, SQLite's own RESTART checkpoint copies the WAL into the database file and the WAL is reused in
     place. Each pass stalls the loop briefly (31–103 ms longest in our load tests) and the WAL stays at ~5.5k
     frames, so no backlog builds up and no requests are held. (`DB_CKPT=uring` instead copies the frames with the
     engine's own checkpoint step on io_uring between loop passes, holding POSTs while a large WAL drains.)
5. **Reply.** Responses are built in a per-batch arena, with headers written in front of each body. Nothing is
   handed to the kernel until the batch's `COMMIT` has returned. If the commit fails, every reply that touched the
   database becomes a 500.
6. **Memory valve.** Connections are never closed to save memory. If a backlog of unread requests builds up while
   memory is short, queued requests get a fast 503 until it drains (see the rules disclosure). Accepts pause only at
   the descriptor limit. (`CURSED_CONN_POLICY=1` brings back `Connection: close` shedding: a fraction of replies,
   rising from none at 400 MB available to half at 200 MB. Each close costs a reconnect, and in our load tests that
   cost more than it saved.)

## Rules disclosure
Each item, and how it stays within the rules:
- **Shared read transaction per batch** (as Rust #82, Go #72). The requests of one wakeup read one snapshot and
  see that batch's own earlier writes. Every reply, reads included, is handed to the kernel only after the
  batch's `COMMIT` returns, and if the commit fails every reply that touched the database becomes a 500, so no
  client ever sees uncommitted data. Nothing outlives the batch.
- **Direct B-tree reads via SQLite's own cursors** instead of SQL text. It's the same database, file format and
  results (differentially tested against the spec's queries on all 500k ids). Writes go through SQL.
- **Write-coalescing VFS.** The same bytes reach the WAL with the same sync behaviour as `synchronous=NORMAL`
  (WAL mode). Only the number of `pwrite` calls changes, and every byte of a commit is written before the commit
  returns and before any response.
- **One small SQLite patch**, applied by `build.sh` to the pinned amalgamation: `pcache1-maxkey`, a pcache
  bookkeeping fix that keeps a commit from scanning the whole page-cache hash after a B-tree page is rekeyed. The
  file format, query results, WAL bytes and sync points are identical to stock (gate-tested), the stock CLI opens
  the file cleanly, and SQLite's own TCL test suite (1,520 files, ~1M cases) and sqllogictest give the same
  pass/fail results as stock 3.54.1.
- **Borrowed, tested, rejected: #154's `balance_nonroot()` one-liner.**
  - *The idea:* #154 spotted that `balance_nonroot()`'s temporary page number (PENDING_BYTE) inflates pcache1's
    `iMaxKey` and forces a full hash walk at the next commit. Its one-line fix uses `pBt->nPage+1` instead. Great
    diagnosis, and it gives the same speed-up as our `pcache1-maxkey`.
  - *The catch:* it passed our challenge-DB gate, but SQLite's own TCL suite fails it. In `savepoint6.test` (tempdb
    variant), from case 315 on, integrity_check reports "Failed to read ptrmap" and queries return "malformed". Stock
    passes all 8,007 cases.
  - *The cause:* under `auto_vacuum=incremental`, `nPage` can be below the pager's `dbSize` inside a transaction. The
    temporary page can then be a dirty cached page, which gets dropped.
  - *Why it didn't bite:* it's latent here, because the challenge schema doesn't use auto-vacuum.
  - *What we did:* we ship `pcache1-maxkey`, which fixes the same thing from the pcache side and passes the full suite.
  - *Lesson:* a gate proves equivalence only for the configurations it exercises.
- **Group commit** (rule 6 allows it): every response waits for its batch's commit.
- **`Connection: close` shedding** (`CURSED_CONN_POLICY=1`, **off** by default: at 450k
  it caused ~80k reconnects and a worse tail; with it off the server held 475k). When on: on a 2 GB box the kernel's ~4.3 KB per open TCP connection is
  the real limit, so the server watches the memory the kernel reports (`/proc/meminfo` MemAvailable − SReclaimable,
  and its cgroup's headroom), averaged over about a second. While at least 400 MB is available it sheds nothing.
  Below that it closes first on a fraction of complete responses (`Connection: close`, nothing pipelined behind
  them), rising linearly to half of them at 200 MB, so clients reconnect instead of the OOM killer ending every
  connection. It looks only at memory, never at who the client is or at CPU. Two emergency floors sit below it and
  are never reached in a passing run: under 100 MB it stops accepting, and under 60 MB it FIN-closes keep-alives
  idle for 20 s or more. `CURSED_SHED_MODE=band` restores the earlier rule (a memory-derived connection cap with a
  shedding band near it), and `CURSED_CONN_POLICY=0` turns shedding off.
- **Bounded checkpoints** (`DB_CKPT=bounded`, the default in `CURSED=max`). SQLite's own `sqlite3_wal_checkpoint_v2`
  in RESTART mode, called on the event loop every 4,096 WAL frames after the batch's replies are submitted, like Rust
  #82's checkpoint. Nothing is held or reordered: requests that arrive during a pass wait in the loop like any other
  backlog.
- **Our own checkpoint step** (`DB_CKPT=uring`, the earlier default, one flag away). The engine re-implements SQLite's WAL
  checkpoint: the same steps in the same order (sync the WAL, copy each page's latest committed frame into the
  database file, sync the database, then advance the backfill mark in the WAL index), producing the same file format.
  Only the I/O differs: it's issued as io_uring file operations between loop passes instead of blocking calls.
  It's tested byte-identical against SQLite's own checkpoint, and ~28k kill and power-cut crash images all recover
  consistently. `DB_CKPT=thread` uses SQLite's `sqlite3_wal_checkpoint_v2` on a thread instead, and `CURSED=boring`
  uses stock SQLite's autocheckpoint (it refuses `uring`).
- **Write hold during checkpoints** (`uring` and `thread` only; `bounded` has none). POSTs wait unread (≤ ~1 s, typically tens of ms) while a checkpoint drains
  the WAL. They are then processed and committed normally.
- **Drain-first receive** (`CURSED_DRAIN=1`, on; `0` restores the earlier path). Received bytes are copied out of the
  kernel's socket buffers into compact application buffers right away, and the requests are then processed in chunks
  of up to 256 per batch. It is an I/O change only: every request is parsed, authenticated, committed and answered
  exactly as before, in the same per-connection order, and each response still waits for its batch's `COMMIT`. Its
  effect is that a backlog costs ~0.5 KB per request in our buffers instead of a ~4 KB socket buffer in the kernel.
- **Socket-pressure 503s, the memory valve** (on; `CURSED_SOCK503_MBPS=0` turns off the socket trigger, and
  `CURSED_DRAIN=0` the backlog trigger with drain-first). When the event loop falls behind, unread requests pile up,
  and at the CPU limit that can reach the OOM killer within seconds. So while available memory is under 450 MB, the
  server raises a pressure flag if the socket-buffer memory charged to its cgroup (`memory.stat` `sock`) rises by more
  than 50 MB within a second while above 64 MB, or passes 160 MB (sampled before each batch, at most every 50 ms), or,
  with drain-first, if its own backlog passes 20,000 requests or its oldest queued request has waited 300 ms (checked
  before each batch). While the flag is up, queued requests, oldest first in arrival order, get an immediate `503
  {"error":"overloaded"}` on the same keep-alive connection, without JWT or database work. It drops once socket memory
  is under 48 MB (or memory recovers) and the backlog is under half those limits. It looks only at memory and queue
  length, never at the route, the client or the request's content, and it doesn't fire under normal load: it sent no
  503s in the local runs at 400k and 425k users. At 450k–475k it does real work, and without it 475k runs out of
  memory. Its 503s are capped by an error budget (`CURSED_SOCK503_BUDGET`, 0.6 % of requests answered, plus a
  20,000-request burst): once the bucket is empty, the valve stops and requests are served normally, so it can never
  turn an overload into more errors than that. Uncapped, it sent up to 1.9 % in 450k runs on a slower CPU; capped, it
  sent ~0.3 % in every run, with the rest of the backlog absorbed by drain-first's buffers.
- **Deadline shedding exists but is off** (`CURSED_DEADLINE_MS`, default 0). When on, a request whose bytes waited
  in the kernel's receive queue longer than the deadline gets an immediate `503 {"error":"overloaded"}` instead of
  a slow answer. It looks only at that queue delay, never at the route, the client or the request's content.
- **Feed coalescing exists but is off.** Sharing one feed computation between requests of a batch is compiled in
  behind a switch that defaults to off, pending a ruling on rule 5.
- **JWT verified on every request**, with `exp` required. Only the key's HMAC ipad/opad state is precomputed (it
  depends on the secret alone). The payload JSON is first matched against the claim layout the spec defines
  (`sub`, `username`, `iat`, `exp`); any other shape goes to the strict general parser. The results are identical
  (fuzz-tested), and `-DJWT_NO_SHAPE_JSON` turns the fast path off.
- **No caches:** no response, query-result or token caches. SQLite's page cache is 8 MiB, and mmap is off.
- **Cached at open:** schema metadata (root pages, column layout) and prepared statements, which hold query plans,
  not data.
- **PGO training** uses a synthetic database generated at build time (own random data, no seed data) and tokens
  signed with a random secret.
- **Rule 7:** nothing detects the load generator. The parser's canonical request lines come from the API spec, and
  any other byte layout takes the general path.
