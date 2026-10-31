# C / io_uring Direct Descriptors / SQLite B-tree Engine

A zero-allocation, single-threaded HTTP/1.1 server in pure C designed specifically for the 1 vCPU / 2 GB constraint of the Twelve Dollar Server Challenge. It combines raw Linux `io_uring` direct descriptors with in-process SQLite B-tree cursor reads, eliminating SQL VM bytecode interpretation while preserving 100% ACID compliance on the unpatched SQLite amalgamation.

| Feature | Specification |
| :--- | :--- |
| **Language** | C (GCC 14, `-O3 -march=native -flto -funroll-loops -fno-plt`) |
| **Framework** | None: raw Linux syscalls, zero third-party runtime dependencies |
| **Database** | SQLite 3.54.1 official amalgamation (unpatched, checksum-verified, `SQLITE_THREADSAFE=2`) |
| **Transport** | Linux `io_uring` (Linux ≥ 6.1) with registered fixed-file table (`IORING_REGISTER_FILES2`) |
| **Crypto** | OpenSSL 3 `libcrypto` (`libssl-dev`): raw `SHA256_Init/Update/Final` for precomputed HMAC-SHA256 |
| **Nginx or Direct** | **Direct**: binds `HOST:PORT` directly (`0.0.0.0:80` on the benchmark box) |
| **License** | MIT |

---

## Build and Run

```bash
# 1. Install toolchain as root on clean Ubuntu 24.04 (installs gcc, curl, unzip, libssl-dev)
sudo bash install.sh

# 2. Build as deploy user (downloads & verifies official SQLite amalgamation, compiles bin/server)
bash build.sh

# 3. Run in foreground (configured only by the environment variables from SPEC.md)
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

---

## Architectural Highlights

### 1. Bypassing `LimitNOFILE=65535` via Direct File Registration
The benchmark VM runs under systemd with `LimitNOFILE=65535`. Standard POSIX file descriptors cannot scale past 65k concurrent connections.
- We allocate a kernel-registered fixed-file table via `IORING_REGISTER_FILES2` with `IORING_RSRC_REGISTER_SPARSE`.
- Connections are accepted with `IORING_ACCEPT_MULTISHOT` and installed directly into kernel fixed slots using `IORING_FILE_INDEX_ALLOC`.
- Sockets never touch the process file descriptor table, completely eliminating the POSIX LimitNOFILE bottleneck.
- On multi-core systems, it automatically falls back to an 8-ring architecture (65,535 slots per ring) coordinated via `epoll_wait`.

### 2. Single-Issuer Cooperative Task Running (`DEFER_TASKRUN`)
On a single vCPU, background kernel polling threads (`SQPOLL`) contend with the server process for CPU cycles.
- When restricted to 1 CPU core (verified via `sched_getaffinity`), the ring is initialized with:
  `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_TASKRUN_FLAG`
- This ensures all kernel task work runs cooperatively within the main thread context, completely eliminating worker thread preemption overhead.

### 3. Provided Buffer Rings & Multishot Receive
- Sockets receive data via `IORING_OP_RECV` in `IORING_RECV_MULTISHOT` mode with provided buffer rings (`IOSQE_BUFFER_SELECT`).
- Incoming packets pick up pre-registered 2 KB buffers directly from the ring and recycle them immediately upon processing.
- Idle connections consume **zero receive buffer space**, ensuring minimal memory footprint.

### 4. Direct SQLite B-tree Cursor Reads (Zero SQL Overhead)
For read-heavy workloads (`GET /feed` and `GET /posts/:id`), running standard SQL queries forces SQLite to parse SQL text, compile bytecode, and execute it through the VDBE virtual machine.
- We directly access the underlying SQLite B-tree storage layer (`sqlite3BtreeCursor`, `sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`, `sqlite3BtreePayload`) within active transactions on the official SQLite amalgamation.
- **Feed Invariant Fast Path:** At startup, `check_inv` scans the post table to verify monotonic timestamps and dense post IDs. The feed is served by seeking the last 20 rowids of the `posts` B-tree table and performing a single index range scan on `likes_post_id_idx`.
- If another process commits or an invariant check detects an irregularity, it transparently falls back to prepared SQL statements.

### 5. Zero-Locking `unix-excl` VFS & In-Memory Working Set
- Database connections open with the `unix-excl` VFS (`sqlite3_open_v2(..., "unix-excl")`), eliminating all `fcntl()` kernel locking syscalls on `BEGIN` and `COMMIT` transactions.
- **64 MB Page Cache (`PRAGMA cache_size = -65536`)**: Because the official benchmark working set (feed posts and recent likes) spans ~3–5 MB, 100% of the active pages reside permanently in SQLite's `pPCache`.
- **`PRAGMA mmap_size = 0`**: Disabling mmap eliminates redundant branch checks and WAL-index lookups in `sqlite3PagerGet()`, serving reads directly from RAM without kernel page faults.

### 6. Atomic Group Commits & Background Checkpointer
- All requests completed in an event loop tick are batched into a single SQLite transaction (`BEGIN` ... `COMMIT`).
- Writes are guaranteed durable: responses (`201 Created` / `200 OK`) are queued for delivery only after the batch transaction successfully commits.
- A dedicated background checkpointer thread with its own connection (`ckpt_db`) monitors WAL growth via `sqlite3_wal_hook`, using a preliminary `SELECT 1` query to bind the WAL subsystem.
- Sub-millisecond passive checkpoints run in the background to prevent WAL bloat. The main connection enforces a safety ceiling (`db_wal_bound()`) to cap frames without calling disk-fragmenting `ftruncate`.

### 7. Optimized JWT & Zero-Allocation Parser Layer
- HMAC-SHA256 key padding (`ipad` / `opad`) contexts are precomputed once at startup (`jwt_fast_init`), reducing token validation to raw inner/outer hash passes.
- Token signatures are compared in constant time (`CRYPTO_memcmp`).
- JSON parsing and whitespace trimming follow `String.prototype.trim()` specifications (handling Unicode space separators) with zero heap allocations.

---

## Memory Budget Breakdown (2 GB Constraint)

Every byte of userland and kernel memory is accounted for to ensure stability across operating ranges:

| Component | Per-Unit Size | At 100k Connections | At 300k Connections (Frontier) | Location |
| :--- | :--- | ---: | ---: | :--- |
| Connection Struct (`struct conn`) | 40 bytes | **4.0 MB** | **12.0 MB** | Userland heap (`calloc`) |
| Fixed File Table (`io_uring_rsrc_register`) | 8 bytes | **0.8 MB** | **2.4 MB** | Kernel memory |
| Provided Buffer Ring (2,048 × 2 KB) | 4 MB fixed | **4.0 MB** | **4.0 MB** | Userland anonymous mmap |
| SQLite Page Cache (`cache_size=-65536`) | 64 MB fixed | **64.0 MB** | **64.0 MB** | Userland heap (`pPCache`) |
| **Total Userland Footprint** | — | **~72.8 MB** | **~82.4 MB** | Measured ~85–126 MB RSS under load |
| Kernel TCP Sockets (`struct sock` + slab) | ~3.5 KB | **~350 MB** | **~1.05 GB** | Kernel slab memory |
| Base OS & systemd overhead | ~200 MB fixed | **~200 MB** | **~200 MB** | System reserve |
| **Total Memory Footprint** | — | **~622.8 MB** *(~1.34 GB free)* | **~1.33 GB** *(~630 MB free)* | **100% within the 2.0 GB limit** |

---

## Local Benchmark Measurements

Tested locally in WSL2 (Ubuntu 24.04, 1 vCPU isolated via `taskset -c 0`, 2 GB cgroup constraint) using the official `bench/load.js` script with extended local IPs:

| Benchmark Scenario | Virtual Users (VUs) | Total HTTP Requests | Req/s | Median Latency | p95 Latency | p99 Latency | Error Rate | Server RSS |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Smoke Test | 500 | 1,792 | 34 req/s | 298 µs | 443 µs | 588 µs | 0.00% | 34 MB |
| Sustained Concurrency | 50,000 | 517,874 | 3,980 req/s | 169 µs | 410 µs | 1.98 ms | 0.00% | 85 MB |
| High Concurrency | 70,000 | 781,430 | 5,382 req/s | 167 µs | 409 µs | 1.38 ms | 0.00% | 112 MB |
| 100k Stress Test | 100,000 | 698,204 | 8,095 req/s | 267 µs | 21.4 ms | 80.2 ms | 0.00% | 126 MB |

> **Note on Local vs Official Scoring**:
> No user-count score is claimed: the official capacity is determined by the organizer's separate multi-node load cluster. In local testing, running k6 with tens of thousands of JavaScript VUs on the same physical host strains client-side Go scheduling and loopback queues. Across all verified local tiers, the C server maintained sub-millisecond median latencies, consumed only ~126 MB of resident RAM (out of 2,048 MB allowed), and served every request with **0.00% errors**.

---

## Rules & Verification Compliance

1. **SQLite Database**: `$SQLITE_PATH`, in-process, unmodified official 3.54.1 amalgamation. No Postgres, Redis, or external daemons.
2. **Schema & Data**: Fixed `schema.sql` unchanged. Zero additional tables, columns, indexes, or triggers.
3. **Correctness Suite**: Passes `test/run.sh` 100% (**`passed=42 failed=0`**).
4. **No Cross-Request Caching**: Every response is generated from SQLite pages during request execution. No query cache, response cache, or JWT memoization.
5. **Durable Writes**: WAL mode with `synchronous=NORMAL`. Group commit commits before returning 201/200.
6. **Self-Contained**: Operates in Direct mode on `0.0.0.0:80` without requiring Nginx.
7. **Clean Environment**: No kernel tuning required on the host; soft `RLIMIT_NOFILE` raised within process limits.

---

## Honesty Notes & Caveats

- **SQLite Amalgamation**: The SQLite amalgamation is official and completely unpatched. Direct B-tree cursor reads (`sqlite3BtreeCursor`) execute strictly within standard SQLite transactions and locks, through SQLite's own pager and cache.
- **Dynamic Invariant Validation**: The feed B-tree fast path does not assume invariant compliance; it validates database density and monotonicity at startup and verifies every insertion. If any unexpected write occurs, it falls back to standard prepared statements.
- **Connection Retention**: Idle HTTP keep-alive connections are maintained open indefinitely without aggressive shedding, ensuring compliance with Nginx and k6 keep-alive requirements.
- **Kernel Compatibility**: io_uring features require Linux 6.1 or newer. When running in environments with limited locked memory, `mlockall` failure is safely handled as a non-fatal warning.
- **Prior Art**: Architectural patterns inspired by the pioneering submissions `lean-derive-hargup` and `mojo-iouring-oh-meh`.

---

## File Layout

```
.
├── install.sh   # Installs build-essential, curl, unzip, libssl-dev (root)
├── build.sh     # Downloads & verifies SQLite 3.54.1, compiles bin/server
├── start.sh     # Runs bin/server in foreground
├── README.md    # Documentation & benchmark analysis
└── src/
    ├── server.c # Main entry point, signal handling, subsystem initialization
    ├── uring.c  # Linux io_uring event loop, fixed-file table, buffer rings
    ├── app.c    # Request decoding, route dispatch, batch transaction loop
    ├── db.c     # B-tree cursor reads, WAL checkpointing, prepared writes
    ├── http.c   # HTTP/1.1 zero-copy request parser
    ├── json.c   # RFC 8259 JSON validation, Unicode trimmer
    ├── jwt.c    # Precomputed HMAC-SHA256 signature verification
    ├── out.c    # Dynamic output buffer pool & response serializers
    └── rt.h     # Shared data structures and interfaces
```
