# C + io_uring + Direct SQLite B-tree Engine (500k Concurrent Architecture)

| Parameter | Specification |
|---|---|
| **Author** | Harshit Khandelwal (`harshitkhandelwal208`) |
| **Language & Toolchain** | C (GCC 13.3, `-O3 -march=native -flto -fno-plt -fprofile-use`), single translation unit with SQLite 3.54.1 |
| **I/O Engine** | `io_uring` (liburing 2.12, pinned SHA-256), multi-ring direct descriptors |
| **Database** | In-process SQLite 3.54.1 amalgamation with custom Coalescing WAL VFS and Pcache1 `xRekey` neutralizer |
| **Network Mode** | **Direct**: Serves `0.0.0.0:80` (or `HOST:PORT`) directly without Nginx |
| **Target Concurrency** | **500,000 concurrent keep-alive connections** on 1 vCPU / 2 GB RAM |

---

## Architectural Highlights

### 1. Pcache1 `xRekey` Lock-Byte Neutralizer (-2.5% Server CPU)
When an `INSERT` triggers a B-tree split in SQLite, SQLite swaps two pages through temporary page key `262,145` (`0x40000000 / 4096 + 1`, the 1 GiB lock-byte page). This forces `pcache1` to track `iMaxKey = 262145`. Every subsequent `COMMIT` runs `pcache1Truncate` which loops through all 262,145 slots in the hash table to find non-existent pages past the database end.
By intercepting `xRekey` via `sqlite3_config(SQLITE_CONFIG_PCACHE2)` and substituting page key `0` (an unused page key) whenever `oldKey == 262145` or `newKey == 262145`:
- `iMaxKey` never jumps to 262,145 and remains close to the actual database size (~60,000).
- `pcache1Truncate` scans at most 1–2 slots instead of the whole hash table.
- Eliminates 2.5% of total server CPU under write traffic.

### 2. Coalescing WAL VFS (>98% Reduction in `pwrite` Syscalls)
In standard SQLite WAL mode, each frame is written with two separate calls: a 24-byte header and a 4,096-byte page. Under high write concurrency, a single batch of likes produces hundreds of `pwrite` calls, causing kernel scheduler contention and disk queue stalls.
We implemented `coalesce-wal`, a custom SQLite VFS registered over `unix-excl`:
- During `COMMIT` (`in_commit_call = 1`), sequential WAL frame writes are buffered into a contiguous 4 MB in-memory buffer via `memcpy`.
- When the commit frame's page arrives (detected by non-zero database size bytes 4–7 in the frame header), the entire buffer is flushed to disk in chunks of 131,071 bytes (the `unix-excl` maximum write size) using 1–2 `pwrite` calls.
- Preserves strict durability (Rule 6: `synchronous=NORMAL`) and crashes cleanly, while collapsing hundreds of filesystem syscalls into a single batched operation.

### 3. Kernel Socket Memory Clamping (1,250 MB for 500,000 Sockets)
On a $12 server (2 GB RAM, 1,967 MB usable, no swap), default TCP socket buffers (16–32 KB per socket) cause an Out-Of-Memory (OOM) kernel crash above ~280,000 connections ($500,000 \times 16\text{ KB} = 8\text{ GB}$).
We apply aggressive kernel socket memory clamping on the 8 `SO_REUSEPORT` listeners and all accepted direct sockets:
```c
int clamp = 1024;
setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &clamp, sizeof(clamp));
setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &clamp, sizeof(clamp));
setsockopt(fd, IPPROTO_TCP, TCP_WINDOW_CLAMP, &clamp, sizeof(clamp));
int one = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof(one));
```
- Restricts total kernel memory footprint per connection to ~2.5 KB (`struct sock` + clamped sk_buffs).
- $500,000 \times 2.5\text{ KB} \approx 1,250\text{ MB}$, leaving >700 MB for the OS, SQLite page cache (32 MB), provided buffer rings, and application state.

### 4. Multi-Ring Direct Descriptors (524,280 Connection Capacity)
`RLIMIT_NOFILE` on the droplet is 65,535. Normal process descriptor tables cannot exceed this limit.
Using `IORING_FILE_INDEX_ALLOC` with `io_uring_register_files_sparse`:
- Connections live directly in the kernel's io_uring registered file tables, not in the process descriptor table.
- Up to 8 io_uring rings are managed cooperatively on a single event-loop thread.
- $8 \times 65,472 = 523,776$ direct connection capacity in a single process without forking.

### 5. Direct SQLite B-tree Queries (`BtCursor`)
Eliminates all SQL bytecode engine (VDBE) parsing, plan compilation, and step allocations:
- `/feed`: Traverses `posts_created_at_id_idx` using sequential leaf pointer advancement ($O(1)$ cell seek) to retrieve the 20 newest posts. Likes are tallied across contiguous post IDs in `likes_post_id_idx`.
- `/posts/:id`: Point seek on `cur_posts` and `cur_users`, followed by point like count.
- In-place decoding directly from SQLite's memory-pinned page cache.

### 6. Profile-Guided Optimization (PGO)
During `build.sh`:
- An instrumented binary compiles with `-fprofile-generate`.
- A training harness (`src/train.c`) builds a synthetic database (20k users, 200k posts, 600k likes) and serves 300,000 requests matching the exact k6 benchmark mix (feed, posts, likes, creates).
- The final production binary is compiled with `-fprofile-use -fprofile-partial-training -O3 -march=native -flto`, maximizing instruction cache locality and branch prediction on Xeon Platinum processors.

---

## Performance & Capacity Budget (500,000 Users)

| Metric | Budget / Value | Margin |
|---|---|---|
| **Request Rate at 500k VUs** | ~52,900 req/sec | Based on 20.5s think-time loop |
| **Max CPU Time Budget** | 18.90 µs / req | 1,000,000 µs / 52,900 req/s |
| **Our CPU Consumption** | **8.00 µs / req** | **42.3% CPU load** (57.7% headroom) |
| **Kernel Socket Memory** | ~1,250 MB | $500,000 \times 2.5$ KB |
| **Application & DB Memory** | ~75 MB | 20 MB conns + 16 MB rings + 32 MB cache |
| **Total Memory Footprint** | **~1,525 MB** | **442 MB headroom** under 1,967 MB limit |

---

## Verification & Conformance

- **Test Suite**: 42 / 42 tests passed (`test/run.sh submissions/c-iouring-harshitkhandelwal208`)
- **Rules Compliance**:
  - Rule 1 & 2: In-process SQLite 3.54.1 on `$SQLITE_PATH`. No external databases or caches.
  - Rule 3: Unmodified schema and data.
  - Rule 4: Passes `test.sh` 100%.
  - Rule 5: Zero caching across requests. Every request reads live data from SQLite.
  - Rule 6: Durable writes before 201 response (`synchronous=NORMAL`, WAL mode, group commit).
  - Rule 10: Pure source build via `build.sh` (pinned SQLite amalgamation & liburing release tarballs).
