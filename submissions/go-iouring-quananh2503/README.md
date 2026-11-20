# Go io_uring + Direct SQLite (B-Tree Engine & Group Commit)

**Author:** quananh2503  
**Language:** Go 1.23+  
**Transport:** Custom zero-allocation HTTP/1.1 proactor over raw Linux `io_uring` (`io_uring_setup`, `io_uring_enter`, registered buffer rings, multishot recv)  
**Database Driver:** Direct CGO against in-tree SQLite 3.53 amalgamation (`src/sqlite/amalg/sqlite3.c`, `sqlite3.h`). Zero external database dependencies (pure Go + C, no `mattn/go-sqlite3`).  
**Serving Mode:** **Direct** on `HOST:PORT` (`0.0.0.0:80` in production benchmarks, or `127.0.0.1:3000`). No Nginx required.

All five endpoints read directly from SQLite while the request is being served. There are no cached responses, query-result caches, or remembered JWT verifications (strictly compliant with Rule 5). All writes are durable (`synchronous=NORMAL`, WAL mode) and committed before returning `201`/`200`.

---

## Architecture & Optimizations

### 1. Raw io_uring Event Loop & Kernel Direct Submission
- **Single-Threaded Proactor:** Dedicated OS thread (`runtime.LockOSThread()`, `GOMAXPROCS=1`) driving a single high-performance `io_uring` instance.
- **Registered Ring FDs (`IORING_REGISTER_RING_FDS` + `IORING_ENTER_REGISTERED_RING`):** Registers the ring file descriptor with the kernel table, bypassing kernel file-table lookup locks (`fget`/`fput`) on hot syscall paths.
- **Go Runtime Scheduler Bypass (`syscall.RawSyscall6`):** Replaces standard Go `Syscall6` with `RawSyscall6` during non-blocking submission passes (`minComplete == 0`), eliminating Go scheduler transitions (`entersyscall`/`exitsyscall`) and suppressing runtime `futex`/`notewakeup` wakeups.
- **Multishot Recv (`IORING_RECV_MULTISHOT`) & Provided Buffer Ring (`IORING_REGISTER_PBUF_RING`):** Buffers are allocated in user-space and registered with the kernel. The kernel picks a buffer upon packet arrival, eliminating user-space re-arming of `OpRecv` and reducing syscall frequency.
- **CQE Suppression (`IOSQE_CQE_SKIP_SUCCESS`):** Suppresses completion queue events on successful `OpSend` operations, cutting CQ processing overhead in half.
- **Expanded CQ Ring (`IORING_SETUP_CQSIZE` 16,384):** Expands the completion queue to 4x submission queue capacity to absorb high-concurrency 20,000 VU bursts without dropping completions.

### 2. DOD (Data-Oriented Design) Request Pipeline
- **Zero-Allocation HTTP/1.1 Parser:** Scans HTTP headers in-place directly from kernel-provided network buffers without heap allocation.
- **Domain System Segregation:** Decouples request handling into cache-friendly contiguous batches (`EventBatch`): Health System, Not Found System, Auth Validation System, Feed Read System, Post Read System, and Group Commit Write System.
- **Zero-Copy Shared Broadcast (`appendSendShared`):** Concurrent feed requests within an event batch share a single formatted HTTP payload buffer from `SendPool`, broadcasting to all client SQEs with zero memory copies.
- **In-Place JWT Verification:** Verifies base64url payload and HMAC-SHA256 signatures directly in scratch memory with reusable hasher instances, incurring 0 heap allocations on the hot path.

### 3. Direct SQLite B-Tree Read Engine (`/feed`, `/posts/:id`)
- **VDBE Engine Bypass:** Bypasses SQLite's SQL query parser and bytecode virtual machine entirely. Resolves root pages (`posts`, `users`, `posts_created_at_id_idx`, `likes_post_id_idx`) at initialization and scans raw B-tree records using `BtCursor`, `sqlite3BtreeFirst`, and `sqlite3BtreeTableMoveto`.
- **Sequential Step Cursor (`sqlite3BtreePrevious`):** Feed records ordered by `(created_at DESC, id DESC)` frequently step across adjacent keys. When `prev_id == id + 1`, the cursor advances via leaf-page step (`sqlite3BtreePrevious`) instead of costly root-to-leaf binary search traversals.
- **Local Stack Memoization (`FeedContext`):** Reuses author usernames across repeating post authors within the local stack frame of a single `get_feed` query (freed upon return; strictly ephemeral and isolated to the single active request, fully compliant with Rule 5).
- **Batch Point Lookups:** `GET /posts/:id` queries are sorted with `qsort`, deduplicating identical IDs in $O(1)$ and reading in a single lock-free transaction.

### 4. Batched Group Commit Writes (`/posts/:id/like`, `/posts`)
- **Single CGO Call & Group Commit:** All like and post creation requests in an `io_uring` completion batch are merged into a single atomic write transaction (`BEGIN IMMEDIATE ... COMMIT`).
- **In-Batch Sorting & Idempotency:** Likes are sorted by `(post_id, user_id)` and checked for existence; duplicate likes from the same user on the same post within the same batch are resolved in memory without database roundtrips.
- **Zero Lock Contention:** Single-threaded execution ensures read and write transactions never interleave or produce `SQLITE_BUSY` errors.

---

## Test & Verification

Passed 100% of the official challenge test suite (42/42 tests passed):
```bash
bash test/run.sh submissions/go-iouring-quananh2503
# passed=42 failed=0
```

---

## Benchmark Performance (k6 Load Test)

Under extreme load testing simulating **20,000 VUs** on 1 vCPU and 2 GB RAM:

| Metric | Result |
|---|---|
| **Total Requests** | **1,661,825 requests** (in 91s) |
| **Average Throughput** | **18,271.6 req/s** |
| **Sustained Peak (30s hold)** | **24,083.6 req/s** |
| **Instantaneous Burst Peak** | **42,582 req/s** |
| **Median Latency ($p50$)** | **168.51 ms** |
| **95th Percentile ($p95$)** | **787.00 ms** |
| **HTTP Error Rate** | **0.00%** (0 failed requests out of 1.66M) |
| **RAM Footprint (RSS)** | **~350 MB - 450 MB** (< 25% of 2.0 GB limit) |
