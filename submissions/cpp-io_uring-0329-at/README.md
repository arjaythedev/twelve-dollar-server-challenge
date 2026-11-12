# C++23 (io_uring, SQLite B-tree reads)

## Language / deps
- C++23, g++ 13 (Ubuntu 24.04 build-essential)
- SQLite 3.54.1 amalgamation, downloaded and compiled by `install.sh` / `build.sh`
- simdjson 5.1.1 (xmake-repo, pinned)
- picohttpparser v1.5 (commit-pinned in `install.sh`)
- liburing 2.5, OpenSSL 3

## Nginx or direct
**Direct.** Serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box).

## Design
One process, one thread, one io_uring ring (`SINGLE_ISSUER | DEFER_TASKRUN`,
SQ 4096, CQ 65536). Eight `SO_REUSEPORT` listeners so a connection burst spreads
over eight accept queues instead of one. Multishot accept (512 armed) and
multishot receive over a provided-buffer ring: an idle connection holds no
buffer.

Each event-loop pass is one SQLite transaction. Responses are built into a
shared bump arena (16 MiB, flushed every 8 MiB) and sent only after COMMIT
returns, so a 201 never claims an uncommitted write. If COMMIT fails the batch is
rolled back and every request of that batch receives a 500.

`GET /feed` and `GET /posts/:id` read SQLite's own B-tree cursors
(`src/sqlite_shim.c` includes the unmodified amalgamation): index walk on
`posts_created_at_id_idx` for the 20 newest ids, records decoded by hand,
authors by rowid in `users`, like counts from one range walk of
`likes_post_id_idx` (falling back to a seek per post when ids are sparse).
Every page still goes through SQLite's pager, WAL and page cache; only the
bytecode interpreter is skipped. The stored layout is checked at startup and any
surprise falls back to the reference SQL, so the bytes on the wire stay golden.

WAL with `synchronous=NORMAL`, opened through the `unix-excl` VFS (one process
owns the file, so the WAL index lives in heap and no fcntl is taken per
transaction), `mmap_size=0`, 64 MiB page cache, automatic checkpoint every 4096
frames (16 MiB).

`src/util.hpp` is `constexpr` throughout and verifies itself at build time with
`consteval`/`static_assert`: base64url, JSON escaping, response framing, integer
formatting, id parsing, trimming, UTF-8 length.

## Correctness
42/42 with `test/test.sh`; `SHIM_CURSORS=1` is set by `start.sh`.

## Local numbers (8 vCPU container, 20k VUs, server pinned to one core)
p95 1.87 ms, p99 46.76 ms, 0 errors, 1592 req/s, RSS 82 MB.

## Build
    bash install.sh
    bash build.sh
    bash start.sh