# Go io_uring + Direct SQLite (B-Tree Engine & Group Commit)

**Language:** Go 1.23+  
**Transport:** Custom zero-allocation HTTP/1.1 parser over raw Linux `io_uring` (`io_uring_setup`, `io_uring_enter`, batch submission/completion queues)  
**Driver:** Direct CGO against in-tree SQLite 3.53 amalgamation (`src/sqlite/amalg/sqlite3.c`, `sqlite3.h`). Zero external database dependencies (pure Go + C, no `mattn/go-sqlite3`).  
**Serving:** Direct on `HOST:PORT` (`0.0.0.0:80` in production benchmarks, or `127.0.0.1:3000`); no Nginx required.

All five endpoints read from SQLite while the request is being served. There are no cached responses, query-result caches, or remembered JWT verifications. All writes are durable (`synchronous=NORMAL`, WAL mode) and committed before returning 201/200.

---

## Architecture & Optimizations

### 1. Raw io_uring Event Loop
- Operates on a single event loop thread with 4,096 entries on the ring buffer.
- Ring-buffered network I/O: Batches `accept4`, `recv`, and `send` operations into single `io_uring_enter` syscalls.
- Pre-allocated arena buffer pools (`RecvPool` with 40,000 slots x 2KB, `SendPool` with 16,384 slots x 16KB) ensure zero dynamic memory allocation on the hot path.

### 2. Direct SQLite B-Tree Read Engine (`/feed`, `/posts/:id`)
- **VDBE Bypass:** Bypasses SQLite's SQL parser and bytecode virtual machine entirely. Resolves root pages (`posts`, `users`, `posts_created_at_id_idx`, `likes_post_id_idx`) at startup and scans raw B-tree records using `BtCursor`, `sqlite3BtreeFirst`, and `sqlite3BtreeTableMoveto`.
- **Feed Coalescing:** Concurrent feed requests in an event batch share a single zero-copy B-tree scan.
- **Batch Point Lookups:** `GET /posts/:id` sorts queries using `qsort`, deduplicating identical IDs in $O(1)$ pointer reuse, and executes point lookups in a single read transaction.

### 3. Batched Group Commit Writes (`/posts/:id/like`, `/posts`)
- **Single CGO Call & Group Commit:** All like and post creation requests in an io_uring completion batch are merged into a single write transaction (`BEGIN IMMEDIATE ... COMMIT`).
- **In-Batch Sorting & Existence Reuse:** Likes are sorted by `(post_id, user_id)`. Existence checks for repeated `post_id`s in the batch are verified once against the B-tree and reused.
- **In-Batch Idempotency:** Duplicate likes from the same user on the same post within the same batch are resolved in memory without database roundtrips.
- **Single-Threaded Isolation:** Running on a single event-driven CPU loop guarantees that read and write transactions never collide, eliminating `SQLITE_BUSY` errors without lock contention.

### 4. Zero-Allocation In-Memory JWT Validation
- Fast-path base64url decoding and HMAC-SHA256 signature verification directly from network receive buffers without allocating heap objects.

---

## Test & Verification

Passed 100% of the official challenge test suite:
```bash
PORT=3125 bash test/run.sh submissions/go-iouring-quananh2503
# passed=42 failed=0
```

---

## Ongoing Optimization & Investigation (Current State)

### 1. Phân Tích Điểm Nghẽn (Profiling & Analysis)
- **Go pprof & Linux Perf Findings:** 
  - Toàn bộ tầng SQLite B-tree & CGO chỉ chiếm **~5-9%** CPU của ứng dụng.
  - HTTP Parser chỉ chiếm **< 0.25%** CPU.
  - Vòng lặp giao tiếp `io_uring` (`SubmitAndRecieve` / `Syscall6`) chiếm tới **>90%** CPU của tiến trình Go. Do đó, tối ưu thêm SQLite hay Parser không còn ý nghĩa.

### 2. Cải Tiến Đã Thực Hiện (Step 1)
- **Single-Syscall (`SubmitAndRecieve`):** Gộp chung quá trình nộp task (SQ) và thu hoạch kết quả (CQ) vào duy nhất 1 lần gọi `io_uring_enter`.
- **Cờ Kernel (`DEFER_TASKRUN` + `SINGLE_ISSUER` + `SUBMIT_ALL`):** Bật các cờ nhân Linux và khóa cứng goroutine vào OS thread (`runtime.LockOSThread()`) để triệt tiêu việc Kernel ngắt CPU không đồng bộ khi nhận packet mạng.
- **Kết quả đo thực tế (20.000 VUs):**
  - Tranh chấp spinlock kernel (`native_queued_spin_lock`) giảm 70% (từ 14.47% xuống 4.49%).
  - Độ trễ đuôi $p99$ giảm 50% (từ 1.28s xuống **653.47 ms**), chính thức đạt chuẩn SPEC ($p99 < 1.0s$).
  - Error rate: **0.00%** (0 lỗi trên 247.535 requests).

### 3. Kiểm Thử Bão Hòa Tải Cao (~3.800 – 5.000 req/s) & Phát Hiện Điểm Gãy
- Khi ép tải lên đỉnh ~5.000 req/s (20.000 VUs, `SLEEP_FACTOR=0.35`):
  - Phục vụ **490.069 requests** với **0.00% lỗi**, median latency đạt **362.54 µs**.
  - Tuy nhiên, độ trễ $p99$ dội lên **1.20s** (vượt chuẩn 1.0s).
- **Nguyên nhân cốt lõi:** Cơ chế Ping-Pong 2 pha (`OpSend` hoàn thành sinh `EventSend` $\to$ đánh thức vòng lặp chỉ để nộp lại `OpRecv`). Ở 5.000 req/s, hàng nghìn micro-batch rỗng sinh ra mỗi giây tạo ra "cửa sổ mù" (blind window) trên các socket đang gửi response, khiến 1% request đuôi bị tích lũy trễ trong hàng đợi TCP của Kernel.

### 4. Kế Hoạch & Tiến Độ Cải Tiến (Milestones)
- [x] **Provided Buffer Ring (`IORING_REGISTER_PBUF_RING`):** Đăng ký bể chứa buffer dùng chung giữa Go và Kernel, trả buffer bằng atomic ở User-space (0 syscall).
- [x] **Multishot Recv (`IORING_RECV_MULTISHOT`):** Treo lệnh đọc vĩnh viễn trên socket từ lúc accept; không bao giờ cần re-arm lại `OpRecv` thủ công sau mỗi request.
- [x] **Bật cờ `IOSQE_CQE_SKIP_SUCCESS` (1 << 6):** Bỏ qua hoàn toàn CQE khi gửi response thành công (`OpSend`) và khi đóng socket (`OpClose`). Triệt tiêu 100% các sự kiện CQE vô ích trên đường gửi.
- [x] **Circular Send Buffer Pool (Ring Arena):** Tái cấu trúc bộ đệm gửi response theo mô hình xoay vòng O(1) với con trỏ `head & mask`, loại bỏ chi phí quản lý LIFO stack và không phụ thuộc vào `EventSend` CQE.

