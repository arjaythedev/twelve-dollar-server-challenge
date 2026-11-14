# C + io_uring + SQLite B-tree Engine (Direct Descriptors & Zero-Stall Checkpoint)

A high-performance C11 server built on Linux `io_uring` and in-process SQLite 3.54.1, serving the HTTP API directly on port 80 without external proxies. Engineered for maximum microsecond-level efficiency, deterministic sub-millisecond p99 latency, and scalable socket scaling on a 1 vCPU / 2 GB RAM server.

| | |
|---|---|
| **Author** | Harshit Khandelwal (`harshitkhandelwal208`) |
| **Language** | C11, GCC 13 (`-O3 -march=native -flto -fno-plt -fprofile-use`), single translation unit with SQLite amalgamation |
| **I/O Subsystem** | Linux `io_uring` (`liburing 2.12`, SHA-256 pinned) with `IORING_SETUP_DEFER_TASKRUN`, `SINGLE_ISSUER`, direct descriptors, and provided-buffer rings |
| **Database** | SQLite 3.54.1 amalgamation (unmodified, SHA-256 pinned), `unix-excl` VFS, 64 MB page cache on huge pages |
| **HTTP / JSON / Auth** | Custom zero-allocation HTTP/1.1 framing, RFC-8259 strict JSON parser, constant-time HMAC-SHA256 with precomputed key schedules |
| **Network Mode** | **Direct**: Serves `0.0.0.0:80` (or `HOST:PORT`) directly without Nginx |
| **Test Conformance** | 42/42 tests passing (`test/run.sh`) with 100% byte-for-byte exactness |

---

## Architectural Principles

### 1. Single Translation Unit & Cross-Inlining
`src/server.c` `#include`s the unmodified SQLite amalgamation (`sqlite3.c`) directly. This enables GCC's link-time optimization (LTO) and compiler optimizer to inline hot SQLite B-tree primitives (`findCell`, `sqlite3BtreeFirst`, `sqlite3BtreeNext`, `sqlite3BtreeTableMoveto`, `getCellInfo`) directly into the request handlers, eliminating all foreign-function interface (FFI) boundary and indirect function call overheads.

### 2. Zero-Stall Background Checkpoint State Machine
Under heavy continuous write traffic, WAL checkpointing on a single vCPU is the primary source of p99 latency spikes and queue collapses. If a checkpoint thread runs synchronously or starves the event loop, p99 latency explodes past 1,000 ms.

We implement a non-blocking, two-phase checkpoint state machine coordinated via zero-cost atomic polling between passes (`td_tick()`):
1. **Bulk Phase (`CK_BULK`)**: Triggered when `wal_frames >= 4096` (16 MB). A dedicated background worker thread running at `nice 19` on its own connection (`ckdb`) executes `SQLITE_CHECKPOINT_PASSIVE` during CPU idle slices. All incoming reads and writes continue at 100% line rate.
2. **Tail Phase (`CK_TAIL`)**: When bulk copying completes (`CK_BULK_DONE`), the event loop sets `writes_held = 1` and triggers the tail phase. Because the bulk phase already copied >99% of WAL frames, the tail checkpoint only has to copy the remaining few dozen frames—a task that completes in **< 5 ms**.
3. **Targeted Write Admission**: While `writes_held == 1`, only incoming `POST` requests (writes, <8% of traffic) are held in their connection input buffers; all `GET` requests (reads, >92% of traffic) continue without interruption. Once the tail finishes, the WAL wraps, `writes_held` is cleared, and held POSTs are released immediately.
4. **Safety Timeout**: If disk I/O stalls, `HOLD_MAX_MS` (250 ms) automatically clears `writes_held`, ensuring no request ever approaches latency thresholds.

### 3. Direct SQLite B-tree Traversal (`BtCursor`)
Eliminates all SQL query parsing, bytecode (VDBE) compilation, expression evaluation, and column allocation:
- **`GET /feed`**: Seeks `posts_created_at_id_idx` (`(created_at DESC, id DESC)`) from the first row and advances the cursor 19 times for the 20 newest posts. Post IDs are sorted in ascending order so that rowid lookups on `cur_posts` hit consecutive cells on the same leaf page (`sqlite3BtreeTableMoveto` fast path). Like counts are fetched via a single range walk of `likes_post_id_idx` when IDs span $\le 512$, or via point seeks otherwise. Author usernames are retrieved from `cur_users` by rowid.
- **`GET /posts/:id`**: Single rowid seek on `cur_posts`, author lookup on `cur_users`, and like count on `cur_likes`.
- **`POST /posts/:id/like`**: Point existence seek on `cur_posts` (returning 404 immediately if absent), followed by prepared `INSERT INTO likes ... ON CONFLICT DO NOTHING`.
- **`POST /posts`**: Validates RFC 8259 JSON, trims JavaScript whitespace, validates UTF-8 code point length ($\le 500$), and executes prepared `INSERT INTO posts ... RETURNING id, created_at`.
- **Output Streaming**: Responses are formatted directly into the batch output arena (`arena`), formatted body-first with zero intermediate allocations.

### 4. Direct Descriptor Rings & Connection Scaling
Under `LimitNOFILE=65535`, standard file descriptors cannot scale past 65k connections:
- **Direct Descriptors**: Accepted sockets live directly in io_uring's fixed file table (`IORING_FILE_INDEX_ALLOC`), completely bypassing the process file descriptor table.
- **Multi-Ring Scaling**: Sized to $65,472$ slots per ring (`RLIMIT_NOFILE - 63`). The server dynamically manages up to 10 rings, accommodating over **650,000 concurrent direct descriptors**.
- **Listen Queue Resilience**: 8 `SO_REUSEPORT` listeners share port 80. Each maintains 64 single-shot direct accepts in flight (512 total across listeners). Accepting automatically rolls over to ring $k+1$ when ring $k$ has fewer than 1,024 free slots.
- **Memory Footprint**: Connections use `conn_t` (48 bytes per connection). Sockets are clamped to `SO_RCVBUF=2048`, `SO_SNDBUF=2048`, `TCP_WINDOW_CLAMP=2048`, `TCP_NODELAY=1`, and `TCP_DEFER_ACCEPT=1`. Total memory per connection in kernel + user space is ~2.8 KB.

### 5. Provided-Buffer Rings & Burst Protection
- Each ring maintains a provided buffer ring of 2,048 x 1,024 byte buffers (`IORING_REGISTER_PBUF_RING` with Launchpad #2162843 fallback). Idle connections hold zero receive buffers in user space.
- Active connections use multishot receive (`io_uring_prep_recv_multishot`).
- **Burst Drain Queue**: When a connection exhausts buffers (`ENOBUFS`) or ends multishot receive, it is placed on a first-in, first-out read queue (`readq`). Bounded rounds of single-shot `MSG_DONTWAIT` receives drain the sockets into memory without triggering ENOBUFS storms.

### 6. Profile-Guided Optimization (PGO)
`build.sh` drives two-stage Profile-Guided Optimization:
1. Compiles an instrumented binary with `-fprofile-generate`.
2. Runs `src/train.c`, which builds a synthetic database and executes 300,000 synthetic requests matching the exact k6 benchmark probability distribution (46% `/feed`, 46% `/posts/:id`, 7% `/like`, 1% `/posts`).
3. Compiles the final production binary with `-fprofile-use -fprofile-partial-training -O3 -march=native -flto -fno-plt`, placing hot instructions in optimal cache lines and tuning branch prediction.

---

## Memory & Resource Budget

| Subsystem | Sizing & Configuration | Memory Footprint |
|---|---|---|
| **SQLite Page Cache** | 64 MB (`CACHE_KB=65536`), huge pages | 64 MB |
| **io_uring Buffer Rings** | 2,048 x 1,024 B per active ring | ~16 MB |
| **Connection Tables** | 48 B x 65,472 per active ring | ~25 MB |
| **Batch Arena & Buffers** | Preallocated pinned arena | 4 MB |
| **Kernel Socket Slab** | Clamped at ~2.8 KB / connection (300k conns) | ~840 MB |
| **Total Memory** | Peak usage under 300,000+ connections | **~950 MB** |
| **Droplet Headroom** | 1,967 MB limit - ~950 MB peak | **> 1,000 MB reserve** |

---

## Conformance & Rules Compliance

- **Rule 1 & 2**: SQLite database file at `$SQLITE_PATH`, operated purely in-process. No Redis, Memcached, or external caching services.
- **Rule 3**: Fixed SQLite schema and seed database. Zero schema modifications, index additions, or triggers.
- **Rule 4**: 100% pass on all 42 tests in `test/test.sh`.
- **Rule 5**: **No cross-request caching.** Every read is serviced from SQLite's page cache and B-tree cursors during the request. Every JWT signature is verified with HMAC-SHA256 in real time.
- **Rule 6**: **ACID Durability.** WAL mode with `synchronous=NORMAL`. Group commit commits all batch writes before any 201 response is released to the network.
- **Rule 7**: Genuine server implementation with strict RFC-8259 JSON validation, JavaScript whitespace trimming, and UTF-8 validation.
- **Rule 8 & 9**: Direct on `0.0.0.0:80` without Nginx.
- **Rule 10**: Pinned dependencies compiled from source (`liburing 2.12` and `sqlite3 3.54.1`), with deterministic SHA-256 validation.
- **Rule 11**: Zero OS or kernel tuning assumptions. Dynamically raises soft `RLIMIT_NOFILE` to hard limit within the process.
