# rust-uring-theCatWisel

Single-threaded multi-ring `io_uring` HTTP/1.1 server in Rust + C with SQLite 3.54.1 in WAL mode.

## Stack

- **Language / Runtime:** Rust `1.94.0` (zero external Cargo dependencies) + C (`sqlite3.c` 3.54.1 amalgamation, 2-stage GCC PGO)
- **Networking:** Direct on `0.0.0.0:80` (no reverse proxy), 8 `SO_REUSEPORT` TCP listeners (`backlog = 4096`), up to 8 lazy `io_uring` rings (`65,472` sparse direct descriptors per ring = `523,776` connection slots)
- **Database:** Unmodified SQLite 3.54.1 (`WAL`, `synchronous = NORMAL`, `cache_size = -4096`)

## Architecture & Optimizations

1. **Lazy Multi-Ring `io_uring` Event Loop (`src/uring.rs`)**
   - Ring 0 runs with `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | TASKRUN_FLAG`. Rings `1..7` are initialized on demand as connection count grows, using `SINGLE_ISSUER` and signaling Ring 0 via `eventfd` multishot polls so completions across all rings are reaped straight from shared memory without extra `io_uring_enter` calls.
   - Idle connections rest on `IORING_OP_POLL_ADD` (`IORING_POLL_ADD_MULTI`, `POLLIN | POLLRDHUP`) holding zero userspace receive buffers.
   - Readable connections enter a bounded FIFO read queue (`READS_PER_BATCH = 16,384`) and issue `MSG_DONTWAIT` single-shot receives selecting from a `4,096 x 1,024 B` kernel provided-buffer ring (`IORING_REGISTER_PBUF_RING`) in the same batch wakeup, while capping SQLite answers per turn (`ANSWERS_PER_BATCH = 2,048`) so socket `sk_buff` memory is drained immediately without starving latency.
   - Disconnects on idle sockets (`POLLHUP | POLLERR | POLLRDHUP`) bypass receive-buffer allocation and close inline via `IORING_OP_ASYNC_CANCEL` + `IORING_OP_CLOSE` without spawning `io-wq` worker threads.

2. **SQLite B-Tree Cursor Engine & Write-Coalescing VFS (`csrc/sqlite_engine.c`)**
   - Uses unmodified `sqlite3.c` 3.54.1 compiled in a single translation unit with our query driver and custom VFS (`astraea-wal-excl`).
   - All requests in an `io_uring` batch share a single SQLite read/write transaction and commit once before responses are flushed to the rings.
   - Custom VFS coalesces consecutive 4 KB WAL frame + header `xWrite` calls into a single `pwrite` syscall at `COMMIT` and caches WAL/DB file sizes in user space.
   - Background checkpoint thread runs `sqlite3_wal_checkpoint_v2(PASSIVE)` and immediately drops checkpointed WAL pages from the OS page cache via `posix_fadvise(..., POSIX_FADV_DONTNEED)` so WAL file pages never crowd out socket memory inside the 1,750 MB cgroup limit.
   - Custom `sqlite3_pcache_methods2` wrapper skips redundant page-cache hash-table scans on WAL commits when no page numbers exceed the current database size.
   - `GET /feed` uses a monotonic tail fast-path (`feed_tail_batch`) when `created_at` order matches `rowid` order (verified once at startup via `PRAGMA integrity_check` + index comparison and maintained across `POST /posts`), reading the last 20 rows from the `posts` table B-tree, sorting author and post IDs to prefetch `pcache1` buckets in a single `sqlite3PcacheFetchStress` pass, and counting likes directly on the `likes_post_id_idx` leaf cell array (`lpid_cell`). Falls back automatically to `posts_created_at_id_idx` (`feed_idx_batch`) if any out-of-order timestamp exists.
   - `POST /posts` generates UTC `strftime('%Y-%m-%dT%H:%M:%fZ')` timestamps in user space from `CLOCK_REALTIME` and formats the `201 Created` JSON directly from the inserted values.

3. **Zero-Dependency HTTP, JSON & Pre-Keyed SHA-NI JWT (`src/http.rs`, `src/jwt.rs`)**
   - Precomputes HMAC-SHA256 `ipad` and `opad` SHA-256 midstates at startup from `JWT_SECRET` and dispatches to hardware `SHA-NI` instructions when available (verified via startup self-test against scalar SHA-256).
   - SIMD-accelerated HTTP/1.1 framing and streaming JSON cursor validate all `SPEC.md` rules without heap allocations on the hot path.
