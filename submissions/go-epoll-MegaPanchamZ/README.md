# go-epoll-MegaPanchamZ

Single-threaded Linux `epoll` HTTP/1.1 server in Go with a C11 + SQLite batch engine.

## Stack

- **Runtime**: Go 1.26 (`cgo`) + C11 (`shim.h`, `pgo/engine.c`)
- **Database**: SQLite 3.53.4 compiled with `pgo/engine.c` as a single PGO-optimized translation unit (`-O3 -march=native`)
- **Mode**: Direct (`0.0.0.0:80` by default; `HOST` and `PORT` configurable)

## How It Works

- **Bounded batches through `cgo`**: Go parses ready HTTP requests and hands each batch to C. If the response arena fills, C commits the completed prefix and Go sends it before executing the unfinished suffix. Create responses reserve space before inserting, so splitting cannot repeat a write. Multi-statement reads share a snapshot; writes share a `BEGIN IMMEDIATE` group commit, completed before replies are sent.
- **Unity SQLite build**: `engine.c` and `sqlite3.c` compile together as one C translation unit so C code reads result columns and binds parameters directly on SQLite's internal `Vdbe` structs instead of going through `sqlite3_column_*` / `sqlite3_bind_*` wrapper layers.
- **Single range-scan `/feed`**: Instead of running 20 separate `COUNT(*)` subqueries on `likes`, `/feed` fetches the 20 latest post IDs (`min_id..max_id`) and counts all likes across that ID window in one contiguous index scan.
- **Fast writes**: Timestamps (`YYYY-MM-DDTHH:MM:SS.mmmZ`) are formatted in C once per millisecond using integer math instead of calling SQLite's `strftime()` on every insert, and new post IDs are read directly from `db->lastRowid` without `RETURNING` ephemeral B-trees.
- **Resident B-tree cache (`pcache1`)**: SQLite runs in exclusive WAL mode with `mmap_size = 0` and a 256 MiB page cache (`cache_size = -262144`), keeping hot `users`, `posts`, and `likes` B-tree pages in memory across multi-trial runs (~60 MB RSS).
- **Zero-copy HTTP & SIMD JSON**: HTTP headers, JWT claims, and JSON bodies are parsed in a single pass without heap allocations. Responses are serialized in C using SSE2 / ARM64 NEON string escaping, with HTTP headers prepended directly in front of the JSON body.
- **Graceful FD eviction**: An intrusive LRU list evicts idle keep-alive sockets in $O(1)$ time when file descriptors approach `ulimit -n`, preventing `EMFILE` accept stalls.

## Capacity and Latency Changes

- Accept at most 128 sockets and parse at most 16 requests per connection per event-loop turn. A ready queue resumes buffered requests without waiting for another socket event.
- Reuse owned transport buffers in a pool capped at 8 MiB. Buffered input is capped at 32 MiB overall; pending output is capped at 2 MiB per socket and 32 MiB overall. Sockets exceeding those limits are closed. Partial writes advance an offset, retaining the original buffer for reuse.
- Track pending responses and connection generations, preserving pipelined response order and rejecting stale events after file descriptor reuse. `100 Continue` uses the same ordered output queue.
- Reclaim idle sockets after 125 seconds and stalled partial requests or output after 120 seconds, with a sweep every five seconds. Timeout accounting uses a monotonic clock.
- Check request and response arena capacity. For a noncontiguous feed within a 256-ID window, serialize only the selected 20 posts. Bound scratch usage and fall back to the reference query when necessary.
- Preserve the passive WAL checkpoint threshold of 262,144 frames (~1 GiB with 4 KiB pages) so multi-trial benchmarks never stall on mid-run checkpoint I/O.

## Local Benchmarks (`taskset -c 0`, 1 Core @ 5.75 GHz, 64 Keep-Alive Connections)

All submissions below were built from source with their full PGO/LTO pipelines and benchmarked back-to-back on the same local machine (pinned to 1 core @ 5.75 GHz on an AMD Ryzen 9 9950X) against the 500k-post / 2M-like `seed/feed.db`.

### 1. 3-Trial Mixed Workload (`100:100:15:2`, Uniform-Random 100k Post IDs)

Three consecutive trials (`2s` warmup + `5s` measured each) against a single running server and database instance:

| Submission | Stack | Trial 1 | Trial 2 | Trial 3 | **Median Req/s** | p50 | p95 | p99 | Errors | **vs `MegaPanchamZ`** |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`#72 go-epoll-MegaPanchamZ`** | **Go + C11** | **222,928** | **221,483** | **204,197** | **221,483 req/s** | **283 µs** | **338 µs** | **375 µs** | **0** | **Baseline (#1)** |
| `#143 goose-native-tbsvttr` | Goose + C | 112,965 | 116,816 | 115,280 | 115,280 req/s | 337 µs | 487 µs | 536 µs | 0 | **+92.1% (1.92×)** |
| `#82 rust-epoll-scooter1337` | Rust + C | 91,475 | 99,584 | 112,434 | 99,584 req/s | 413 µs | 841 µs | 1,165 µs | 0 | **+122.4% (2.22×)** |
| `#44 go-stdlib-vkrishna03` | Go + C | 78,675 | 100,396 | 95,767 | 95,767 req/s | 577 µs | 680 µs | 749 µs | 0 | **+131.3% (2.31×)** |
| `#64 c-epoll-TheFirstIstari` | Pure C11 | 64,064 | 77,134 | 72,368 | 72,368 req/s | 598 µs | 1,038 µs | 1,484 µs | 0 | **+206.1% (3.06×)** |

### 2. Per-Endpoint Breakdown (`taskset -c 0`, 64 Connections)

| Workload | `#143 goose-native` | `#82 rust-epoll` | `#44 go-stdlib` | `#64 c-epoll` | **`#72 go-epoll-MegaPanchamZ`** |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **`GET /feed`** | 139,105 req/s | 122,891 req/s | 73,725 req/s | 57,132 req/s | **149,567 req/s** (p99 **0.49 ms**) |
| **`GET /posts/:id`** | 376,264 req/s | 364,651 req/s | 232,494 req/s | 275,303 req/s | **378,068 req/s** (p99 **0.20 ms**) |
| **`POST /posts/:id/like`** | 262,642 req/s | 290,690 req/s | 133,585 req/s | 191,128 req/s | **333,599 req/s** (p99 **0.23 ms**) |
| **`POST /posts`** | 12,594 req/s (p99 256 ms) | 11,068 req/s (p99 281 ms) | 4,332 req/s (p99 579 ms) | 8,279 req/s (p99 137 ms) | **17,637 req/s** (p99 **0.53 ms**) |
| **Mixed (`100:100:15:2`)** | 175,460 req/s (p99 0.35 ms) | 156,312 req/s (p99 0.43 ms) | 133,448 req/s (p99 0.49 ms) | 103,683 req/s (p99 0.87 ms) | **271,545 req/s** (p99 **0.31 ms**) |

### 3. File-Descriptor Pressure (`ulimit -n 256`)

| Scenario (`ulimit -n 256`) | PR #52 (`damianoneill`) | PR #65 (`Antheurus`) | **`go-epoll-MegaPanchamZ`** |
| :--- | ---: | ---: | ---: |
| **800 rotating keep-alive clients (2,400 reqs)** | 2,400/2,400 in 1,035 ms | 2,400/2,400 in 1,794 ms | **2,400/2,400 in 442 ms (2.34× faster)** |
| **250 idle keep-alive holders + 200 new clients** | 0/200 (stalled on `EMFILE`) | 0/200 (stalled on `EMFILE`) | **200/200 (100%) in 3 ms** |
