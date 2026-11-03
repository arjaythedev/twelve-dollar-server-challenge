# go-epoll-MegaPanchamZ

Single-threaded Linux `epoll` HTTP/1.1 server in Go with a C batch SQLite execution and SIMD JSON serialization engine.

## Stack & Mode

- **Language / Runtime**: Go 1.26 (`cgo`) + C11 (`shim.h`, `pgo/engine.c`)
- **Database**: SQLite 3.53.4 compiled in a single Unity translation unit with `pgo/engine.c` into `libsqlite3.a` using Profile-Guided Optimization (`-fprofile-generate` / `-fprofile-use`), `-O3 -march=native -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_OMIT_FOREIGN_KEY -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_DECLTYPE`
- **Serving Mode**: **Direct mode** (`HOST` defaults to `0.0.0.0`, `PORT` defaults to `80` in `start.sh`; overrides respected by `test/run.sh`)

## Architecture & Key Optimizations

### 1. Unity PGO Translation Unit & Single-`cgo`-Call-Per-Batch Execution (`db_exec_batch`)
In standard `cgo` SQLite servers, each `epoll_wait` batch crosses the Go↔C boundary dozens of times (`sqlite3_bind_*`, `sqlite3_step`, `sqlite3_column_*`, `sqlite3_reset`), and each `sqlite3_column_*` / `sqlite3_bind_*` call traverses multiple wrapper layers (`columnMem` -> `sqlite3_value_*` -> `columnMallocFailure` -> `sqlite3ApiExit`, or `vdbeUnbind` -> `sqlite3VdbeMemRelease`).

`go-epoll-MegaPanchamZ` compiles `sqlite3.c` and [`pgo/engine.c`](./pgo/engine.c) together as a **single Unity C translation unit** trained with Profile-Guided Optimization (`pgo/train.c`), and executes all parsed requests from an `epoll_wait` wakeup in **one `cgo` call** (`C.db_exec_batch`):
- **Direct `Vdbe` / `Mem` Column, Parameter & Rowid Access**: Because `engine.c` and `sqlite3.c` share a translation unit (`HAVE_SQLITE_AMALGAMATION`), result columns are read directly from `((Vdbe *)stmt)->pResultRow[i]` (`u.i`, `z`, `n`), parameters are bound directly in `((Vdbe *)stmt)->aVar[i - 1]`, and inserted IDs are read from `db->lastRowid`, eliminating ~250 wrapper function calls per `GET /feed`.
- **Zero-`strftime` Write Path & Ephemeral B-Tree Elimination**: `format_iso8601_ms` formats the 24-byte UTC timestamp (`YYYY-MM-DDTHH:MM:SS.mmmZ`) in C once per millisecond using Howard Hinnant's civil-from-days integer math and binds it statically (`SQLITE_STATIC`) to `INSERT INTO posts` and `INSERT OR IGNORE INTO likes`. This eliminates SQLite's `strftime()` C function invocation on every write, removes the `RETURNING id, created_at` ephemeral B-tree (`OpenEphemeral` + second `sqlite3_step`) on `POST /posts`, and cuts duplicate-like VDBE execution from 46 opcodes down to 17.
- **Shared Read Snapshots & Write Group Commit**: Multi-statement read batches share a single `BEGIN DEFERRED ... COMMIT` transaction across the batch, while single-statement `OP_GET_POST` batches execute directly in autocommit mode (1 `sqlite3_step` instead of 3). Batches containing `POST /posts` or `POST /posts/:id/like` open a single `BEGIN IMMEDIATE ... COMMIT` transaction so all writes in the batch are durably committed to the WAL before any HTTP response is sent.

### 2. 3-Range-Scan `GET /feed` Algorithm
The reference `GET /feed` query executes 20 correlated `SELECT count(*) FROM likes l WHERE l.post_id = p.id` index scans (one B-tree traversal of `idx_likes_post_id` per post).
Because `posts.id` is `INTEGER PRIMARY KEY AUTOINCREMENT` and `created_at` is monotonically non-decreasing on creation, the top 20 posts in `/feed` almost always occupy a narrow contiguous ID range `[min_id, max_id]`:
1. Scan `idx_posts_created_at` (`SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20`) to collect the 20 post IDs (`min_id..max_id`).
2. When `max_id - min_id < 256`, execute a **single contiguous index range scan** (`SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2`) and accumulate like counts into a stack array `uint32_t likes[256]`, replacing 20 separate B-tree seeks with 1 sequential leaf scan.
3. Scan `posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC` directly by rowid. If the 20 IDs are strictly contiguous descending (`ids[i] == max_id - i`), serialize each row directly into the output arena; if out-of-order within the 256-ID window, format into scratch buffers and assemble in exact `ids[0..19]` order; if `max_id - min_id >= 256`, fall back transparently to the full reference query.

### 3. Single-Pass AVX2 HTTP/1.1 & RFC 8259 JSON Parsing
- **Non-Blocking `SYS_EPOLL_PWAIT` Fast Poll**: When active, the event loop polls `SYS_EPOLL_PWAIT` with timeout `0` via `syscall.RawSyscall6` before falling back to blocking `syscall.EpollWait`, bypassing Go's `runtime.entersyscall` / `runtime.exitsyscall` scheduler transitions under load.
- **Single-Pass `IndexByte` HTTP Parser & Inline ASCII Header Match**: Scans request and header lines in a single forward pass using Go's SIMD `bytes.IndexByte(..., '\n')` and matches header names via inline word-comparison (`eqFoldASCII`) instead of UTF-8 rune-decoding `bytes.EqualFold`.
- **Single-Pass Validating `objectWalker` & Zero-Copy Unescaping**: Validates RFC 8259 JSON object syntax while extracting JWT claims (`sub`, `username`, `exp`, `nbf`) and `POST /posts` bodies in a single pass. Uses `bytes.IndexByte(inner, '\\') < 0` (`unescapeRef`) to return unescaped string slices zero-copy directly from the request/token buffer whenever no backslash escape is present.

### 4. In-Place HTTP Framing, 2-Digit Lookup `u64toa` & SSE2 / ARM64 NEON SIMD JSON Escaping
- **SSE2 & ARM64 NEON String Escaping & 2-Digit Integer Table**: Post bodies and usernames are escaped in 16-byte vectors via SSE2 (`_mm_loadu_si128` on x86_64) or ARM64 NEON (`vld1q_u8` / `vmaxvq_u8` on `aarch64`), checking for `"`, `\`, and control bytes (`< 0x20`) in parallel. Integer serialization (`u64toa`) uses a single-branch fast path for `< 10` (`like_count`) and a 200-byte lookup table (`digits2`) for 2-digits-at-a-time division.
- **Zero-Copy HTTP Header Prepending**: Each operation writes its JSON payload into the C output arena starting at `pos + 128`, computes the exact `Content-Length`, and prepends the HTTP/1.1 response headers directly in front of `body_start` (`body_start - hdr_len`), eliminating intermediate body buffers and `memcpy` passes.

### 5. Resident `pcache1` (`-DSQLITE_MAX_MMAP_SIZE=0`) & Lazy-Clock LRU Eviction
- **Pure `pcache1` B-Tree Page Cache**: In SQLite WAL mode, enabling `mmap_size > 0` routes every page fetch through `getPageMMap`, which does not insert clean pages into `pcache1` (forcing `sqlite3WalFindFrame` hash probes on every B-tree lookup) and falls back to `pcache1` inside `BEGIN IMMEDIATE` write batches whenever pages are dirtied in the WAL. Compiling SQLite with `-DSQLITE_MAX_MMAP_SIZE=0` and configuring `PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA mmap_size = 0; PRAGMA cache_size = -262144; PRAGMA wal_autocheckpoint = 0;` keeps all hot `users`, `posts`, and `likes` B-tree pages resident in `pcache1` across multi-trial mixed workloads with zero `pread64` syscalls and zero mid-trial blocking WAL checkpoints (~60 MB RSS).
- **Two-Tier FD Pressure Control**:
  - Above `softMaxFDs`, responses append `Connection: close` so keep-alive clients cycle out gracefully after their current request.
  - At `hardMaxFDs`, `evictIdle()` evicts idle keep-alive connections in $O(1)$ amortized time from the intrusive doubly-linked list (`lruHead` / `lruTail`), preventing `EMFILE` accept stalls when concurrent keep-alive clients exceed the OS file descriptor limit.

## Local Head-to-Head Benchmark Results (1 CPU Core, `taskset -c 0`, 64 Keep-Alive Connections)

All submissions below were built from source with their full PGO/LTO pipelines and benchmarked back-to-back on the same machine (`taskset -c 0`, 64 keep-alive connections, official 500k-post / 2M-like `seed/feed.db`):

### 1. 3-Trial Consecutive Mixed Workload (`100:100:15:2`, Uniform-Random 100k Post IDs on a Single DB Instance)

Simulates the multi-trial benchmark structure (`2s` warmup + `5s` measured trial × 3 consecutive trials on the same running server and database):

| Submission | Stack | Trial 1 | Trial 2 | Trial 3 | **Median Req/s** | p50 | p95 | p99 | Errors | **`go-epoll-MegaPanchamZ` Lead** |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`#72 go-epoll-MegaPanchamZ`** | **Go + C11 (`epoll` + PGO)** | **186,012** | **189,376** | **191,340** | **189,376 req/s** | **328 µs** | **414 µs** | **625 µs** | **0** | **Baseline (#1)** |
| `#143 goose-native-tbsvttr` | Goose + C (`epoll` + 2-stage PGO/LTO) | 112,965 | 116,816 | 115,280 | 115,280 req/s | 337 µs | 487 µs | 536 µs | 0 | **+64.3% faster (1.64×)** |
| `#82 rust-epoll-scooter1337` | Rust + C (`epoll` + PGO + fat LTO) | 91,475 | 99,584 | 112,434 | 99,584 req/s | 413 µs | 841 µs | 1,165 µs | 0 | **+90.2% faster (1.90×)** |
| `#44 go-stdlib-vkrishna03` | Go + C (`net/http` + `cgo`) | 78,675 | 100,396 | 95,767 | 95,767 req/s | 577 µs | 680 µs | 749 µs | 0 | **+97.7% faster (1.98×)** |
| `#64 c-epoll-TheFirstIstari` | Pure C11 (`epoll` + PGO + LTO) | 64,064 | 77,134 | 72,368 | 72,368 req/s | 598 µs | 1,038 µs | 1,484 µs | 0 | **+161.7% faster (2.62×)** |

### 2. Per-Endpoint & Mixed Workload Breakdown (`taskset -c 0`, 64 Keep-Alive Connections)

| Workload | `#143 goose-native` | `#82 rust-epoll` | `#44 go-stdlib` | `#64 c-epoll` | `#52 go-epoll` | `#65 go-epoll` | **`#72 go-epoll-MegaPanchamZ`** |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`GET /feed`** | 137,547 req/s | 118,944 req/s | 69,910 req/s | 55,688 req/s | 57,776 req/s | 48,518 req/s | **132,032 – 140,976 req/s** (**7.09 µs/req**, p99 **0.82 ms**) |
| **`GET /posts/:id`** | 374,471 req/s | 362,928 req/s | 240,191 req/s | 272,758 req/s | 233,528 req/s | 202,830 req/s | **365,197 – 426,207 req/s** (**2.34 µs/req**, p99 **0.24 ms**) |
| **`POST /posts/:id/like`** | 254,574 req/s | 271,157 req/s | 120,829 req/s | 185,352 req/s | 140,251 req/s | 175,816 req/s | **320,188 – 333,030 req/s** (**3.00 µs/req**, p99 **0.28 ms**) |
| **`POST /posts`** | 11,932 req/s (p99 287.6 ms) | 11,203 req/s (p99 309.6 ms) | 3,477 req/s (p99 102.7 ms) | 7,346 req/s (p99 175.4 ms) | 7,567 req/s (p95 92.4 ms) | 14,750 req/s (80.96 µs/req) | **29,001 req/s** (**4.79 µs/req**, p95 **0.65 ms**, p99 **0.92 ms**) |
| **Mixed (`100:100:15:2`)** | 163,946 req/s (p99 0.56 ms) | 130,310 req/s (p99 0.89 ms) | 90,954 req/s (p99 0.81 ms) | 88,057 req/s (p99 1.24 ms) | 82,439 req/s (p99 1.05 ms) | 87,892 req/s (p99 2.23 ms) | **255,275 – 260,019 req/s** (**3.84 µs/req**, p95 **0.28 ms**, p99 **0.37 ms**) |

### 3. FD-Pressure & Keep-Alive Eviction Benchmark (`ulimit -n 256`)

| Scenario (`ulimit -n 256`) | PR #52 (`damianoneill`) | PR #65 (`Antheurus`) | `go-epoll-MegaPanchamZ` |
| :--- | ---: | ---: | ---: |
| **800 rotating keep-alive clients (2,400 reqs)** | 2,400/2,400 in 1,035 ms | 2,400/2,400 in 1,794 ms | **2,400/2,400 in 442 ms (2.34× faster)** |
| **250 idle keep-alive holders + 200 new clients** | **0/200 (0.0%)** — stalled on `EMFILE` | **0/200 (0.0%)** — stalled on `EMFILE` | **200/200 (100.0%) in 3 ms** |
