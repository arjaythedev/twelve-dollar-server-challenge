# c-iouring-Vojtik1112

Single-thread, multi-ring raw Linux `io_uring` HTTP server written in C11, embedding the unmodified SQLite 3.54.1 amalgamation directly in one translation unit (LTO + on-box PGO over a synthetic database).

| | |
|---|---|
| Language | C11 (`gcc` 13 from Ubuntu 24.04 `build-essential`) |
| Framework | None (raw Linux `io_uring` syscalls, custom HTTP/1.1, JSON, HS256) |
| Database / Driver | SQLite 3.54.1 amalgamation (unmodified, SHA-256 pinned, single translation unit, direct B-tree cursors + `"feed-wal"` write-coalescing VFS shim) |
| Build | `-O3 -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -flto=auto`, static, on-box PGO trained on synthetic DB |
| **Nginx or direct** | **Direct** — serves `HOST:PORT` directly (`0.0.0.0:80` on the benchmark droplet, no Nginx) |
| Requirements | Linux 6.1+ (`io_uring` with `SINGLE_ISSUER`, `DEFER_TASKRUN`, provided buffer rings, fixed-file direct descriptors) |

## Architecture

- **Raw `io_uring` Event Loop ([`src/uring.h`](src/uring.h), [`src/server.c`](src/server.c))**:
  - 8 `SO_REUSEPORT` listeners (`backlog=4096`) and 6 pre-created `io_uring` rings (expandable on demand up to 12 rings × 65,471 sparse fixed-file slots = 785k+ direct descriptors, sized from `RLIMIT_NOFILE`) with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG` and `MADV_HUGEPAGE` provided buffer rings (`RECV_BUFS=8192` per ring, 1 KB buffers).
  - Bounded single-shot `ACCEPT` (`ACCEPT_DEPTH=512`, capped strictly within per-ring free slot margin) so ring spill never drops TCP connections with `-ENFILE` during connection bursts.
  - Multishot `RECV` (`IORING_CQE_F_MORE`) for 1-CQE / 0-SQE per-request steady state, with bounded `read_rounds()` (`MSG_DONTWAIT` single-shot draining) on `-ENOBUFS` bursts, periodic 256-CQE buffer-ring publishing, immediate pending-buffer reclaim, and cgroup/PSI listener `SO_RCVBUF` memory governing.
  - Ring 0 polls worker rings' `eventfd`s and the WAL checkpointer's `eventfd` (`UD_CKPT_EFD`) in one `io_uring_enter` wait.
  - Zero-copy inline pipelined request coalescing, adaptive batching (`min_complete=1` at low load, `64` / `2000 us` timeout and `ANSWERS_PER_PASS=1024` under saturation), bounded write-coalescing window (`WRITE_WINDOW_US=2000`), and age-bounded LIFO `ready[]` queue scheduling.
- **Zero-Copy HTTP / AVX2 JSON / Unrolled-64 SHA-256 JWT ([`src/http.h`](src/http.h), [`src/json.h`](src/json.h), [`src/sha256.h`](src/sha256.h), [`src/jwt.h`](src/jwt.h), [`src/app.h`](src/app.h))**:
  - Responses frame directly into a ref-counted 1 MB slab arena with reverse HTTP header prepending and zero-alloc precomputed static error responses (`S_RESP`).
  - AVX2/SSE2 JSON string scan/escape and exact ECMAScript Unicode whitespace trimming (`js_trim`).
  - Table-driven branchless Base64URL signature decoding and PGO/LTO unrolled-64 HMAC-SHA256 verification.
- **Direct B-Tree Readers & SQLite Engine ([`src/btree.h`](src/btree.h), [`src/db.h`](src/db.h))**:
  - **`GET /feed` & `GET /posts/:id`**: Persistent B-tree cursors (`sqlite3BtreeTableMoveto`, `sqlite3BtreeLast`, `sqlite3BtreePrevious`, `sqlite3BtreeNext`), 2-leaf tail pinning with O(1) cell-19 `rank_twenty` check, range/single-post `likes_post_id_idx` counting with +8 lookahead & binary run-length subtraction, dedicated `CUR_USERS` for point lookups, O(1) per-slot `CUR_AUTHOR` cursors with 1-slot cursor rotation on `POST /posts`, fast inline `users` leaf payload decoding, and full SQLite prepared-statement fallback on overflow/unusual layouts.
  - **Direct `FastCache` (`SQLITE_CONFIG_PCACHE2`)**: O(1) `pgno -> page` direct pointer lookup array backed by `MADV_HUGEPAGE` slabs (auto-sized to `98304` KiB on <= 2.5 GB boxes / cgroups so 350k+ TCP sockets fit comfortably in 1.75 GB, or `262144` KiB when memory is plentiful) with priority-ordered index/tail warming (`db_warm`) and clock eviction fallback.
  - **`"feed-wal"` VFS & Carried Read Transactions**: 256 KB sequential WAL write coalescing (flushed in <=64 KB chunks right before `walShmBarrier(pDbFd)` / `xSync` / `xRead` / `xUnlock`), `CARRY_READ_TX=1` across read-only batches, `db_now_iso` fast UTC timestamp formatting, and a two-thread background checkpointer (`nice 5` bulk `PASSIVE` + background `sync_file_range` WAL writeback kicks + `POSIX_FADV_DONTNEED` page-cache drops, then `nice 0` short `RESTART` signaled via `eventfd`).

## Credits & Inspirations

Per challenge Ruling 9, this implementation builds upon public techniques documented in prior submissions in this repository:
- Direct SQLite B-tree cursor traversal, tail fast-path with cell-19 certificate, +8 binary run-length like counting, raw `io_uring` setup, `NLISTEN=8` `SO_REUSEPORT` listeners, listener `SO_RCVBUF` cgroup memory governor, `POSIX_FADV_DONTNEED` page-cache drops, background WAL writeback kicks, and synthetic `--make-train-db` PGO harness patterns from [`c-iouring-jowinjestine`](../c-iouring-jowinjestine) and [`c-uring-willwang`](../c-uring-willwang).
- Bounded single-shot `ACCEPT` across listeners (`ACCEPT_DEPTH`), bounded `read_rounds()` `-ENOBUFS` single-shot draining, and dual-thread background WAL checkpointing patterns from [`rust-iouring-kensac`](../rust-iouring-kensac).
- Arena framing, lock-code hardening, `PCACHE2` direct-slot table ideas, and static response table patterns from [`c-iouring-pavlyhalim`](../c-iouring-pavlyhalim).
- `CARRY_READ_TX`, `CUR_AUTHOR` per-slot feed author cursors, and age-bounded LIFO scheduling from [`fsharp-uring-delneg`](../fsharp-uring-delneg) and [`objc-uring-delneg`](../objc-uring-delneg).
- Pre-hold `fdatasync(db_fd)` + post-sync checkpoint pass and `SQLITE_FCNTL_CHUNK_SIZE` from [`swift-iouring-akbashev`](../swift-iouring-akbashev).
