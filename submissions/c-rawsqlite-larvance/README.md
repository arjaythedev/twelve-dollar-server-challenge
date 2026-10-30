# Zero-Library Native SQLite 3 B-Tree and WAL Engine and io_uring Server

A pure C99 / GNU C implementation for the Twelve-Dollar Server Challenge with zero external libraries (no libsqlite3, no sqlite3.c amalgamation, no OpenSSL, no third-party HTTP/JSON libraries). It implements SQLite 3 on-disk B-tree (0x0d, 0x05, 0x0a, 0x02) and Write-Ahead Log (-wal format 3007000, magic 0x377f0682) formats from scratch on top of a custom multi-ring io_uring transport.

---

## 1. Stack and Versions

| Component | Implementation and Version |
| :--- | :--- |
| Language | C99 / GNU C (gcc 13.x on Ubuntu 24.04, glibc 2.39, Linux 6.8+) |
| HTTP Framework | Custom zero-library multi-ring io_uring HTTP/1.1 keep-alive server (src/uring.c, src/http.c) |
| Database Driver / Engine | Custom zero-library native SQLite 3 B-tree (0x0d/0x05/0x0a/0x02) and WAL (3007000) storage engine (src/raw_sqlite.c, src/sqlite_format.h) |
| JSON and JWT | Custom zero-copy AVX2 JSON parser/serializer (src/json.c, src/out.c) and hardware SHA-NI HS256 JWT verifier (src/jwt.c) |
| External Libraries | None (libc, libpthread, libm only) |

---

## 2. Deployment Mode: Direct (0.0.0.0:80) or Nginx (127.0.0.1:3000)

- Preferred Mode: Direct (HOST=0.0.0.0 PORT=80).
  - The server natively manages up to 131,072 concurrent keep-alive connections across multiple SO_REUSEPORT io_uring instances using direct (fixed) file descriptors (IORING_REGISTER_FILES2) that bypass the process file table and RLIMIT_NOFILE bottlenecks.
  - Running directly on port 80 eliminates Nginx reverse-proxy socket hop and saves about 15-20% CPU on the 2-vCPU Hetzner CPX22 target.
- Nginx Mode: Also fully supported out of the box (defaults to HOST=127.0.0.1 PORT=3000 when environment variables are unset).

---

## 3. Optimizations and Architecture

### A. Zero-Library Native SQLite 3 B-Tree and WAL Engine (src/raw_sqlite.c, src/sqlite_format.h)
Instead of paying SQLite SQL tokenizer/parser, VDBE bytecode interpreter, B-tree cursor mutexes, and pager lock overhead on every request, we read and write SQLite 3 binary file format directly while maintaining 100% on-disk compatibility (sqlite3 CLI PRAGMA integrity_check returns ok at all times):

1. O(1) Unified Page Pointer Table (page_ptrs[MAX_DB_PAGES]):
   - get_page(pno) is a single array load (return page_ptrs[pno];) with zero loops and zero branches, pointing either to the mmap-backed main database file (MADV_WILLNEED | MADV_HUGEPAGE) or to the latest dirty WAL frame buffer in wal_frames_buf.
   - Generation-stamped O(1) per-batch dirty tracking (batch_dirty_mark[pno] == cur_batch_id) avoids linear scans and per-batch memset calls.
2. True SQLite 3 B-Tree Inserts and 2-Level Page Splits (Zero In-Memory Table Copies):
   - Zero in-memory table or count caches are used; 100% of reads and writes operate directly on SQLite 3 B-tree pages and WAL frames.
   - raw_insert_like performs a single-pass binary search and insert on sqlite_autoindex_likes_1 (root_likes_pk), inserts (post_id, rowid) into likes_post_id_idx (root_likes_pid), and appends (user_id, post_id, created_at) to likes (root_likes), with full 2-level B-tree leaf and interior page splitting.
   - raw_insert_post appends rows to posts (root_posts) using a tracked rightmost B+tree cursor with 2-level page splitting.
3. True In-Order Index B-Tree Cursor (raw_index_seek_ge, raw_index_next):
   - Traverses both leaf (0x0a) and interior (0x02) index cells in exact in-order sequence, computing like_count across the 20-post /feed window in a single contiguous index range scan (raw_range_counts).
4. Contiguous WAL Frame Pool and Single-Syscall Group Commit (wal_frames_buf):
   - Dirty pages are modified in-place inside wal_frames_buf[slot] (24B wal_frame_hdr_t + 4096B page).
   - At the end of each io_uring iteration (raw_sqlite_batch_end), all dirty pages across all writes in the batch are committed to app.db-wal with cumulative big-endian SQLite WAL checksums in one single contiguous pwrite() syscall and zero page copies at commit time.
   - A background checkpoint thread (checkpoint_thread_main) flushes dirty frames back to the database mapping and runs fdatasync every 1,000 WAL frames off the critical event-loop thread.
5. Dynamic Schema Root Discovery (discover_schema_roots):
   - Parses Page 1 (sqlite_schema) at startup to dynamically resolve the root pages for users, posts, likes, and their indexes.

### B. Multi-Ring io_uring Transport and SIMD Protocol Engine (src/uring.c, src/http.c, src/out.c, src/jwt.c)
1. Single-Issuer io_uring with Direct Descriptors and Provided Buffer Rings:
   - Uses IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL, multishot IORING_ACCEPT_MULTISHOT, multishot IORING_RECV_MULTISHOT with provided buffer rings (IORING_REGISTER_PBUF_RING), and fixed file slots (IOSQE_FIXED_FILE).
   - Queued OP_SEND SQEs are flushed immediately (COALESCE_US = 0) without artificial pre-send sleep stalls.
2. AVX2 Vectorized HTTP Parsing and JSON Escaping:
   - find_head_end in src/http.c scans 32 bytes at a time using _mm256_cmpeq_epi8 to locate \r\n\r\n.
   - out_jstr in src/out.c scans 32 bytes per instruction for JSON characters requiring escaping (double quote, backslash, or control bytes below 0x20).
   - fmt_u64 formats integers two digits at a time via a 200-byte lookup table (DEC_PAIRS).
3. Hardware SHA-NI HS256 JWT Verification (src/jwt.c):
   - Uses x86 SHA-NI instructions (_mm_sha256rnds2_epu32, _mm_sha256msg1_epu32, _mm_sha256msg2_epu32) with runtime CPU detection (__builtin_cpu_supports("sha")) and a portable scalar fallback, plus precomputed HMAC inner/outer key pads.

---

## 4. Benchmarks (Local Comparison vs PR #153 Winner lean-derive-hargup)

### Native blast Throughput (4 threads, 8 connections = 32 keep-alive connections)

| Workload | Winner (lean-derive-hargup) | This Submission | Speedup |
| :--- | ---: | ---: | ---: |
| GET /feed | 44,578 req/s | 100,821 req/s | 2.26x |
| GET /posts/:id | 57,077 req/s | 139,456 req/s | 2.44x |
| POST /posts/:id/like | 49,334 req/s | 131,571 req/s | 2.67x |
| POST /posts (create) | 3,670 req/s | 129,786 req/s | 35.36x |
| mixed (46/46/7/1%) | 46,845 req/s | 111,959 req/s | 2.39x |

### Official k6 Load Test (bench/load.js)

| Metric | Winner (lean-derive-hargup) | This Submission |
| :--- | ---: | ---: |
| Average Latency (avg) | 755.00 us | 261.29 us |
| Median Latency (med) | 800.00 us | 258.14 us |
| p95 Latency (limit below 500 ms) | 975.00 us | 391.27 us |
| p99 Latency (limit below 1000 ms) | 1,650.00 us | 514.74 us |
| Failed Requests | 0.00% | 0.00% |
| test/test.sh Checks | 42 / 42 | 42 / 42 |
