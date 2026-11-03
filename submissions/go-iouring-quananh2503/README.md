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
