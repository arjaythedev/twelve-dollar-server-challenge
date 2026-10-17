# go-epoll-MegaPanchamZ

Single-threaded Linux `epoll` HTTP/1.1 server in Go with a C batch SQLite execution and SIMD JSON serialization engine.

## Stack & Mode

- **Language / Runtime**: Go 1.26 (`cgo`) + C11 (`shim.h`, `pgo/engine.c`)
- **Database**: SQLite 3.53.4 compiled in a single Unity translation unit with `pgo/engine.c` into `libsqlite3.a` using Profile-Guided Optimization (`-fprofile-generate` / `-fprofile-use`), `-O3 -march=native -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_DECLTYPE`
- **Serving Mode**: **Direct mode** (`HOST` defaults to `0.0.0.0`, `PORT` defaults to `80` in `start.sh`; overrides respected by `test/run.sh`)

## Architecture & Key Optimizations

### 1. Unity PGO Translation Unit & Single-`cgo`-Call-Per-Batch Execution (`db_exec_batch`)
In standard `cgo` SQLite servers, each `epoll_wait` batch crosses the Go↔C boundary dozens of times (`sqlite3_bind_*`, `sqlite3_step`, `sqlite3_column_*`, `sqlite3_reset`), and each `sqlite3_column_*` / `sqlite3_bind_*` call traverses multiple wrapper layers (`columnMem` -> `sqlite3_value_*` -> `columnMallocFailure` -> `sqlite3ApiExit`, or `vdbeUnbind` -> `sqlite3VdbeMemRelease`).

`go-epoll-MegaPanchamZ` compiles `sqlite3.c` and [`pgo/engine.c`](./pgo/engine.c) together as a **single Unity C translation unit** trained with Profile-Guided Optimization (`pgo/train.c`), and executes all parsed requests from an `epoll_wait` wakeup in **one `cgo` call** (`C.db_exec_batch`):
- **Direct `Vdbe` / `Mem` Column & Parameter Access**: Because `engine.c` and `sqlite3.c` share a translation unit (`HAVE_SQLITE_AMALGAMATION`), result columns are read directly from `((Vdbe *)stmt)->pResultRow[i]` (`u.i`, `z`, `n`, `flags & MEM_Null`) and integer parameters are bound directly in `((Vdbe *)stmt)->aVar[i - 1]`, eliminating ~250 wrapper function calls per `GET /feed`.
- **Shared Read Snapshots & Single-Op Fast Path**: Multi-statement read batches share a single `BEGIN DEFERRED ... COMMIT` transaction across the batch, while single-statement `OP_GET_POST` batches execute directly in autocommit mode (1 `sqlite3_step` instead of 3).
- **Write Group Commit**: Batches containing `POST /posts` or `POST /posts/:id/like` open a single `BEGIN IMMEDIATE ... COMMIT` transaction. All writes in the batch are durably committed to the WAL before any HTTP response is sent to client sockets. If a batch commit fails, all operations in the batch are rolled back and `500` responses are returned.

### 2. 3-Range-Scan `GET /feed` Algorithm
The reference `GET /feed` query executes 20 correlated `SELECT count(*) FROM likes l WHERE l.post_id = p.id` index scans (one B-tree traversal of `idx_likes_post_id` per post).
Because `posts.id` is `INTEGER PRIMARY KEY AUTOINCREMENT` and `created_at` is monotonically non-decreasing on creation, the top 20 posts in `/feed` almost always occupy a narrow contiguous ID range `[min_id, max_id]`:
1. Scan `idx_posts_created_at` (`SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20`) to collect the 20 post IDs (`min_id..max_id`).
2. When `max_id - min_id < 256`, execute a **single contiguous index range scan** (`SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2`) and accumulate like counts into a stack array `uint32_t likes[256]`, replacing 20 separate B-tree seeks with 1 sequential leaf scan.
3. Scan `posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC` directly by rowid. If the 20 IDs are strictly contiguous descending (`ids[i] == max_id - i`), serialize each row directly into the output arena; if out-of-order within the 256-ID window, format into scratch buffers and assemble in exact `ids[0..19]` order; if `max_id - min_id >= 256`, fall back transparently to the full reference query.

### 3. Single-Pass AVX2 HTTP/1.1 & RFC 8259 JSON Parsing
- **Single-Pass `IndexByte` HTTP Parser & Inline ASCII Header Match**: Scans request and header lines in a single forward pass using Go's SIMD `bytes.IndexByte(..., '\n')` and matches header names via inline word-comparison (`eqFoldASCII`) instead of UTF-8 rune-decoding `bytes.EqualFold`.
- **Single-Pass Validating `objectWalker` & Zero-Copy Unescaping**: Validates RFC 8259 JSON object syntax while extracting JWT claims (`sub`, `username`, `exp`, `nbf`) and `POST /posts` bodies in a single pass. Uses `bytes.IndexByte(inner, '\\') < 0` (`unescapeRef`) to return unescaped string slices zero-copy directly from the request/token buffer whenever no backslash escape is present.

### 4. In-Place HTTP Framing, 2-Digit Lookup `u64toa` & SSE2 SIMD JSON Escaping
- **SSE2 String Escaping & 2-Digit Integer Table**: Post bodies and usernames are escaped in 16-byte vectors via `_mm_loadu_si128`, checking for `"`, `\`, and control bytes (`< 0x20`) in parallel. Integer serialization (`u64toa`) uses a single-branch fast path for `< 10` (`like_count`) and a 200-byte lookup table (`digits2`) for 2-digits-at-a-time division.
- **Zero-Copy HTTP Header Prepending**: Each operation writes its JSON payload into the C output arena starting at `pos + 128`, computes the exact `Content-Length`, and prepends the HTTP/1.1 response headers directly in front of `body_start` (`body_start - hdr_len`), eliminating intermediate body buffers and `memcpy` passes.

### 5. Zero-Buffer Idle Connections, Lazy-Clock LRU Eviction & Adaptive WAL Checkpointing
- **Zero-Buffer Idle State & Lazy-Clock LRU**: Idle keep-alive connections hold `nil` read/write slices. Active requests only update `c.lastMs = s.nowMs` on the local connection struct (avoiding cross-connection pointer writes on the hot path); `evictIdle()` lazily rotates recently active connections to `lruTail` only when `openConns >= hardMaxFDs`.
- **Two-Tier FD Pressure Control**:
  - Above `softMaxFDs`, responses append `Connection: close` so keep-alive clients cycle out gracefully after their current request.
  - At `hardMaxFDs`, `evictIdle()` evicts idle keep-alive connections in $O(1)$ amortized time from the intrusive doubly-linked list (`lruHead` / `lruTail`), preventing `EMFILE` accept stalls when concurrent keep-alive clients exceed the OS file descriptor limit.
- **Adaptive WAL-Index Checkpointing**: SQLite is configured with `PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA mmap_size = 1073741824; PRAGMA cache_size = -2000;`. Passive WAL checkpoints (`sqlite3_wal_checkpoint_v2`) are triggered after response flushing at `4,000` frames whenever `GET /feed` is active (keeping the WAL index inside a single 4,096-frame / 32 KB L1-resident hash table segment) and `32,000` frames during pure write bursts.

## Head-to-Head Benchmark Results (1 CPU Core, `taskset -c 0`, 64 Keep-Alive Connections)

Measured against the top two existing Go submissions (**PR #52 `go-epoll-damianoneill`** and **PR #65 `go-epoll-Antheurus`**) and the top pure-C `epoll` submission (**PR #64 `c-epoll-TheFirstIstari`**, C11 + PGO + LTO) on the official 500k-post / 2M-like seeded database (`seed/feed.db`):

| Workload | PR #52 (Go `damianoneill`) | PR #65 (Go `Antheurus`) | PR #64 (C11 `TheFirstIstari`) | **`go-epoll-MegaPanchamZ`** | Speedup vs Best Go | Speedup vs Pure C (#64) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`GET /feed`** | 57,776 req/s (17.31 µs/req) | 48,518 req/s (16.40 µs/req) | 57,290 req/s (17.12 µs/req) | **137,751 – 140,976 req/s** (**7.09 µs/req**, p99 **0.84 ms**) | **+144.0%** (2.44×) | **+146.1%** (2.46×) |
| **`GET /posts/:id`** | 233,528 req/s (4.28 µs/req) | 202,830 req/s (4.38 µs/req) | 293,126 req/s (3.41 µs/req) | **423,630 – 426,207 req/s** (**2.34 µs/req**, p99 **0.27 ms**) | **+82.5%** (1.83×) | **+45.4%** (1.45×) |
| **`POST /posts/:id/like`** | 140,251 req/s (7.13 µs/req) | 175,816 req/s (5.12 µs/req) | 179,095 req/s (5.58 µs/req) | **307,838 – 313,314 req/s** (**3.19 µs/req**, p99 **0.38 ms**) | **+78.2%** (1.78×) | **+74.9%** (1.75×) |
| **`POST /posts`** | 7,567 req/s (p95 92.4 ms) | 14,750 req/s (80.96 µs/req, 97.5 MB RSS) | 8,009 req/s (p95 88.1 ms, p99 147.3 ms) | **13,738 req/s** (**7.07 µs/req**, p95 **0.59 ms**, p99 **0.82 ms**, 13.8 MB RSS) | **11.5× lower CPU/req** | **+71.5%** (179× lower p99) |
| **Mixed (`100:100:15:2`)** | 82,439 req/s (9.03 µs/req, p99 1.05 ms) | 87,892 req/s (9.19 µs/req, p99 2.23 ms) | 90,800 req/s (7.75 µs/req, p99 1.07 ms) | **151,933 req/s** (**4.42 µs/req**, p95 **0.45 ms**, p99 **0.60 ms**) | **+72.9%** (1.73×) | **+67.3%** (1.67×) |

### FD-Pressure & Keep-Alive Eviction Benchmark (`ulimit -n 256`)

| Scenario (`ulimit -n 256`) | PR #52 (`damianoneill`) | PR #65 (`Antheurus`) | `go-epoll-MegaPanchamZ` |
| :--- | ---: | ---: | ---: |
| **800 rotating keep-alive clients (2,400 reqs)** | 2,400/2,400 in 1,035 ms | 2,400/2,400 in 1,794 ms | **2,400/2,400 in 442 ms (2.34× faster)** |
| **250 idle keep-alive holders + 200 new clients** | **0/200 (0.0%)** — stalled on `EMFILE` | **0/200 (0.0%)** — stalled on `EMFILE` | **200/200 (100.0%) in 3 ms** |



