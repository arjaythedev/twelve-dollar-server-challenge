# c-iouring-Vojtik1112

Single-thread, multi-ring raw Linux `io_uring` HTTP server written in C11, embedding the unmodified SQLite 3.54.1 amalgamation directly in one translation unit (LTO + on-box PGO over a synthetic database).

| | |
|---|---|
| Language | C11 (`gcc` 13 from Ubuntu 24.04 `build-essential`) |
| Framework | None (raw Linux `io_uring` syscalls, custom HTTP/1.1, JSON, HS256) |
| Database / Driver | SQLite 3.54.1 amalgamation (unmodified, SHA-256 pinned, single translation unit, direct B-tree cursors + `"feed-wal"` coalescing VFS over `"unix-excl"`) |
| Build | `-O3 -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -flto=auto`, static, on-box PGO trained on synthetic DB |
| **Nginx or direct** | **Direct** — serves `HOST:PORT` directly (`0.0.0.0:80` on the benchmark droplet, no Nginx) |
| Requirements | Linux 6.1+ (`io_uring` with `SINGLE_ISSUER`, `DEFER_TASKRUN`, provided buffer rings, fixed-file direct descriptors; optional 6.7+ socket `URING_CMD`) |

## Architecture

- **Raw `io_uring` Event Loop ([`src/uring.h`](src/uring.h), [`src/server.c`](src/server.c))**:
  - 8 `SO_REUSEPORT` listeners (`backlog=4096`) with `TCP_QUICKACK=0` (accepted sockets inherit delayed bare-ACK suppression so responses acknowledge requests) plus `IORING_OP_URING_CMD` (`SOCKET_URING_OP_SETSOCKOPT`) `TCP_QUICKACK=0` refresh on late answers (`>= 30 ms`).
  - 6 pre-created `io_uring` rings (expandable up to 12 rings × 65,471 sparse fixed-file slots) with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG` and `MADV_HUGEPAGE` provided buffer rings (`RECV_BUFS=2048` × 1 KB buffers = 12 MB across 6 rings).
  - Single combined submit+collect enter per active ring (zero worker `eventfd`s, zero mid-pass `io_uring_enter`); Ring 0 reads the checkpointer's `eventfd` via `IORING_OP_READ`.
  - Bounded single-shot `ACCEPT` (`ACCEPT_DEPTH=512`, capped strictly within per-ring free slot margin) so ring spill never drops connections with `-ENFILE`.
  - Multishot `RECV` (`IORING_CQE_F_MORE`) with bounded `read_rounds()` `-ENOBUFS` single-shot draining and periodic 256-CQE buffer-ring publishing.
  - Compact 24-byte `struct conn` (side pool for rare split/pipelined buffers), direct `mmap` 1 MB send arenas (`ARENA_POOL=4`), pre-parsed & JWT-verified `ready[]`/`held[]` queue + bump arena (zero `malloc`/`brk` on steady `pipe=1` requests), bounded `WRITE_WINDOW_US=15000`, `ANSWERS_PER_PASS=256`, age-bounded LIFO scheduling (`LIFO_AGE_MS=8000`), and cgroup/PSI listener `SO_RCVBUF` governing (no idle connection dropping before 300 s).
- **Single-Pass HTTP / Compact AVX2 JSON / Unrolled-64 SHA-256 JWT ([`src/http.h`](src/http.h), [`src/json.h`](src/json.h), [`src/sha256.h`](src/sha256.h), [`src/jwt.h`](src/jwt.h), [`src/app.h`](src/app.h))**:
  - Single-pass HTTP scanner dispatching headers on first byte; precomputed static error responses (`S_RESP`).
  - L1i-compact JSON serializer (fast 16B/scalar short-string path + outlined AVX2 body path) and exact ECMAScript Unicode trimming (`js_trim`).
  - Branchless Base64URL decoding and unrolled-64 HMAC-SHA256 verification.
- **Direct B-Tree Readers & SQLite Engine ([`src/btree.h`](src/btree.h), [`src/db.h`](src/db.h))**:
  - **`GET /feed` & `GET /posts/:id`**: Persistent B-tree cursors (`sqlite3BtreeTableMoveto`, `sqlite3BtreeLast`), speculative 2-leaf tail walk (`CUR_FEED_TAIL` + `CUR_POST_PREV`) with inline leaf cell parser and O(1) cell-19 `rank_twenty` check, O(1) `CUR_LIKES_LAST` in-leaf binary search for `GET /posts/:id` + range `likes_post_id_idx` scan (+8 lookahead & binary run-length subtraction), 32-slot hash-indexed `CUR_USER_BASE[uid & 31]` cursor bank shared between `/feed` and `/posts/:id`, and full SQL fallback on overflow/unusual layouts.
  - **Pure `PCACHE2` `FastCache` (`SQLITE_CONFIG_PCACHE2`)**: O(1) `pgno -> slot` lookup backed by `MADV_HUGEPAGE` slabs (`65536` KiB on `<= 2.5 GB` boxes / cgroups, +512 headroom, pure `xFetch`/`xUnpin` clock eviction without private struct casts) and surgical ~38 MB working-set warmup (`users`, `posts_user_id_idx`, `sqlite_autoindex_likes_1`, post/like tails).
  - **`"feed-wal"` over `"unix-excl"`**: Eliminates per-tx `fcntl` locks and disk `-shm` files; coalesces WAL writes and flushes on commit frame (`bytes 4..7 != 0`) before `COMMIT` completes, paired with `CARRY_READ_TX=1`, `db_now_iso`, and a single `nice 19` background `PASSIVE` checkpointer (with page-cache drops scoped strictly to `$SQLITE_PATH` and `$SQLITE_PATH-wal` and zero read freezes).

## Credits & Inspirations

Per challenge Ruling 9, this implementation builds upon public techniques documented in prior submissions in this repository:
- Direct SQLite B-tree cursor traversal, tail fast-path with cell-19 certificate, +8 binary run-length like counting, raw `io_uring` setup, `NLISTEN=8` `SO_REUSEPORT` listeners, listener `SO_RCVBUF` cgroup memory governor, `POSIX_FADV_DONTNEED` scoping, `unix-excl` VFS + commit-frame WAL coalescing, and synthetic `--make-train-db` PGO harness patterns from [`c-iouring-jowinjestine`](../c-iouring-jowinjestine) and [`c-uring-willwang`](../c-uring-willwang).
- Bounded single-shot `ACCEPT`, bounded `read_rounds()` `-ENOBUFS` single-shot draining, combined multi-ring submit+wait loop, and `nice 19` background `PASSIVE` checkpointing from [`rust-iouring-kensac`](../rust-iouring-kensac).
- Arena framing, binary code pinning + COW fallback, `PCACHE2` direct-slot cache, and static response tables from [`c-iouring-pavlyhalim`](../c-iouring-pavlyhalim) and [`mojo-iouring-oh-meh`](../mojo-iouring-oh-meh).
- `CARRY_READ_TX`, `TCP_QUICKACK=0` + `LATEACK` socket command refresh, multi-slot post/user cursors, growing-WAL passive checkpointing, and age-bounded LIFO scheduling from [`fsharp-uring-delneg`](../fsharp-uring-delneg) and [`objc-uring-delneg`](../objc-uring-delneg).
- `SQLITE_FCNTL_CHUNK_SIZE` and pre-hold `fdatasync` pattern from [`swift-iouring-akbashev`](../swift-iouring-akbashev).
