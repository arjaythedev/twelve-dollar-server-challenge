# C + io_uring

| | |
|---|---|
| Language | C (GCC 13.x on Ubuntu 24.04 x86_64) |
| Framework | None (Custom single-threaded event loop) |
| Async Engine | Linux `io_uring` with registered descriptors and provided buffer rings |
| SQLite Driver | SQLite 3.45.1 amalgamation compiled in-tree with LTO + PGO |
| **Nginx or direct** | **Direct**: Serves `0.0.0.0:80` directly |

All dependencies are built from source via `install.sh` and `build.sh`.

## Running it

```bash
sudo bash install.sh   # apt: gcc, liburing-dev, build-essential, curl, unzip
bash build.sh          # compiles SQLite amalgamation with PGO and links bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Architecture & Optimizations

1. **Single-Threaded io_uring, 1 Core, 0 Locks**:
   The benchmark droplet provides 1 vCPU. Running multiple worker threads introduces context switches and mutex contention over SQLite's write lock. A single thread drives both the `io_uring` event loop (`IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN`) and all SQLite queries sequentially without locks.

2. **Registered Descriptors (`IORING_REGISTER_FILES`)**:
   Pre-registers the socket descriptor table with the kernel so `recv` and `send` SQEs execute with `IOSQE_FIXED_FILE`, avoiding per-packet kernel file-table atomic reference counting. Includes automatic fallback for environments with lower `RLIMIT_NOFILE` limits.

3. **Provided Buffer Rings & 16-Byte Connection Slots**:
   Instead of pre-allocating static read/write buffers per connection, the kernel leases 4 KB read buffers dynamically from a 4,096-entry buffer ring (`IOSQE_BUFFER_SELECT`) only when data arrives on a socket and returns them immediately after parsing. Per-connection state (`conn_slot_t`) is packed into 16 bytes (1.04 MB total for 65,536 connections), with a 512-entry partial-packet pool to reassemble split TCP segments asynchronously.

4. **Single-Pass HTTP Scanner & Zero-`memmove` Serialization**:
   Scans the request line and headers in a single pass using `memchr`, skipping `Authorization` and `Content-Length` header parsing on GET routes. Outgoing JSON payloads are formatted at a 128-byte offset using inline integer-to-ASCII routines, and the HTTP response header is written directly in front of the payload so responses are transmitted without `sprintf` or `memmove`.

5. **Precomputed HMAC-SHA256 Midstates + SHA-NI**:
   Because `JWT_SECRET` is fixed at startup, the inner (`key ^ 0x36`) and outer (`key ^ 0x5c`) 64-byte SHA-256 block compressions are precomputed once at boot. Per-request JWT verification restores those midstates and uses x86 SHA-NI hardware instructions (`#ifdef __SHA__`, with a portable C fallback), cutting per-request HMAC work in half.

6. **Covering-Index Queries & Exclusive WAL Locking**:
   Opens SQLite with `PRAGMA journal_mode = WAL;`, `PRAGMA synchronous = NORMAL;`, and `PRAGMA locking_mode = EXCLUSIVE;` (acquiring the WAL lock once at startup rather than locking/unlocking `.db-shm` on every query). GET /feed walks `posts_created_at_id_idx` in index order (`LIMIT 20`), joins `users` by integer rowid, and counts likes via the covering index `likes_post_id_idx` with zero temporary B-trees or post-sorting. All read cursors call `sqlite3_reset()` immediately so passive WAL checkpoints never block.

7. **Tick-Drained Group Commit & Adaptive Coalescing (Rule 6)**:
   Write requests (`POST /posts` and `POST /posts/:id/like`) arriving within the same `io_uring` completion batch are executed inside a single `BEGIN IMMEDIATE; ... COMMIT;` transaction. Responses (`201 Created` / `200 OK`) are queued for transmission only after `COMMIT` completes. When the event loop is busy (`last_batch_count >= 16`), completions are dynamically coalesced with a 1.0 ms timeout (`io_uring_submit_and_wait_timeout`), batching up to 32 events and reducing syscall churn. Furthermore, UTC ISO-8601 millisecond timestamps are formatted once per batch in C and bound directly to both `posts` and `likes` inserts, eliminating per-row SQLite `strftime()` calls.

8. **Two-Step Like Disambiguation**:
   Executes `INSERT INTO likes ... WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING`. When `sqlite3_changes() == 1`, it returns `201 Created` immediately; only when `changes == 0` does it run `SELECT 1 FROM posts WHERE id = ?` to distinguish duplicate likes (`200 OK`) from missing posts (`404 Not Found`).

9. **Full-Pipeline LTO + Profile-Guided Optimization (PGO)**:
   `build.sh` compiles `server.c` and `sqlite3.c` together as a single LTO unit (`-O3 -flto -fno-plt`) and runs a 50,000-iteration `--pgo-train` pass across all five endpoints so GCC inlines hot branches across the application and SQLite's VDBE/B-tree engine.

## License

MIT, under the repo's [license](../../LICENSE).
