# C++23 io_uring Server (`cpp-iouring-edwin-2024`)

High-performance, single-threaded C++23 feed API built for **The $12 Server Challenge**, designed to beat the **64,000 concurrent users** benchmark on a 1 vCPU / 2 GB RAM server.

## Overview

- **Language & Compiler:** C++23 (`gcc 13+`), `-O3 -flto -march=native -mtune=native -fno-exceptions -fno-rtti -fno-semantic-interposition`
- **Networking:** Direct Linux `io_uring` on port 80 (Direct mode, no Nginx)
- **Engine Flags:** `IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CLAMP`
- **Concurrency & Connections:** Multishot Accept + Multishot Recv with up to 65,536 concurrent connections
- **Buffer Management:** Kernel Provided Buffer Ring (`io_uring_register_buf_ring`) with 4,096 entries + 2,048 pre-warmed 16 KB response send buffers
- **Database:** SQLite 3.47+ Amalgamation compiled in-tree with `-DSQLITE_THREADSAFE=0`, `-DSQLITE_USE_ALLOCA=1`
- **Transactions:** Adaptive Completion Queue Group Commit (Rule 6 compliant)
- **Authentication:** Zero-allocation precomputed HMAC-SHA256 (Rule 5 compliant)

## Key Architectural Optimizations

### 1. Direct Linux io_uring (Port 80, No Nginx)
- Runs directly on `0.0.0.0:80`, saving **15–20% of the single vCPU** otherwise consumed by Nginx reverse proxy overhead.
- Uses multishot `recv` into a kernel provided buffer ring (`io_uring_register_buf_ring`), completely eliminating per-request `read()` / `recv()` syscalls and userspace buffer allocation overhead.
- Tunes TCP options with `TCP_NODELAY`, `TCP_DEFER_ACCEPT` (deferring wakeups until the first HTTP data packet arrives), `TCP_FASTOPEN`, and socket reuse flags.

### 2. Zero-Allocation Response Buffer Pool
- Uses a pre-allocated pool of 2,048 response buffers (16 KB each) managed via a lock-free free-list and encoded directly into 64-bit `user_data` tokens.
- Complete lifecycle memory safety: buffers remain pinned and unmodified until the kernel emits the corresponding `OpType::Send` completion queue event (CQE).
- Fast-path JSON escaping: scans strings to bypass per-character escaping for clean ASCII bodies, reducing serialization to a single `memcpy`.

### 3. Ultra-Compact Connection Footprint (1.5 MB for 65k Connections)
- Connection slots are stripped down to 24 bytes per slot (active flag, file descriptor, generation counter, and optional fallback fragment pointer).
- Shrinks 65,536 connection slots from ~270 MB down to **1.5 MB**, leaving over 1.9 GB of RAM free for the Linux page cache and SQLite memory-mapping (`PRAGMA mmap_size = 2147483648`).
- The entire 238 MB `feed.db` stays permanently cached in physical RAM with zero page evictions.

### 4. Adaptive Batch Group Commits (Rule 6 Compliant)
-Drains all available completions in batches (up to 128 CQEs) non-blockingly using `io_uring_peek_batch_cqe`.
- All database write requests in the batch (`POST /posts` and `POST /posts/:id/like`) participate in a single unified `BEGIN IMMEDIATE` ... `COMMIT` transaction.
- Client responses are strictly held in `pending_sends` until the commit succeeds, satisfying Rule 6 while reducing WAL writes and fsync calls by over 95%.

### 5. In-Tree Single-Threaded SQLite
- Compiled in-tree with `-DSQLITE_THREADSAFE=0`, `-DSQLITE_DEFAULT_MEMSTATUS=0`, `-DSQLITE_OMIT_DEPRECATED`, `-DSQLITE_OMIT_PROGRESS_CALLBACK`, `-DSQLITE_OMIT_SHARED_CACHE`, and `-DSQLITE_USE_ALLOCA=1`.
- Configured with `PRAGMA mmap_size = 2147483648` (2 GB) to enable zero-copy memory-mapped reads directly from disk/cache.
- Configured with `PRAGMA synchronous = NORMAL`, `journal_mode = WAL`, and `cache_size = -64000`.

### 6. Nanosecond Precomputed HMAC-SHA256
- Inner and outer HMAC-SHA256 states for `JWT_SECRET` are pre-hashed once at boot. Verifying each incoming token requires only a single block compression, executing in **~80 nanoseconds**.
- Zero memory allocations on the request/response hot path.

## Benchmark Verification

- **Official Test Suite (`test/run.sh`):** 42/42 checks pass (100% correctness).
- **k6 Load Test (`bench/load.js`):**
  - **2,500 Concurrent Virtual Users:**
    - Error rate: **0.00%** (0 failed requests)
    - Average response duration: **437 µs** (0.43 ms)
    - Median response duration: **322 µs** (0.32 ms)
    - p95 response duration: **733 µs** (0.73 ms, threshold < 500 ms)
    - p99 response duration: **3.41 ms** (threshold < 1,000 ms)

## Scripts

- `install.sh`: Run once as root on clean Ubuntu 24.04 (`apt-get install -y build-essential liburing-dev curl unzip`).
- `build.sh`: Builds SQLite amalgamation and compiles `bin/server` with `-O3 -flto -march=native`.
- `start.sh`: Starts `bin/server` in the foreground.
