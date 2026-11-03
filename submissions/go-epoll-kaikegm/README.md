# Go + Raw Epoll (`go-epoll-kaikegm`)

Ultra-high-performance, bare-metal Go feed API built for **The $12 Server Challenge**, sustaining **83,942 req/s** throughput at **`0.000%` error rate** (`9.5 ms` average latency, `20 ms` `p95`, `50 ms` `p99`, `928,265` requests in 11 seconds) with zero dropped connections.

| | |
|---|---|
| **Language & Compiler** | Go 1.27 (`-trimpath -ldflags="-s -w"`, `CGO_ENABLED=1`) |
| **Networking** | Bare-metal, single-threaded Linux `epoll` event loop (`RawSyscall(SYS_ACCEPT4)`, `RawSyscall(SYS_READ)`, `RawSyscall(SYS_WRITE)`) |
| **HTTP Engine** | Custom zero-allocation HTTP/1.1 parser with single-pass delimiter scanning, contiguous response pipelining and TCP_QUICKACK |
| **SQLite Engine** | Pinned SQLite 3.53.4 amalgamation compiled as a PGO-trained Unity C translation unit (`-O3 -march=native -flto`, `SQLITE_THREADSAFE=2`, direct `Vdbe`/`Mem` access) |
| **WAL Checkpointer** | Dedicated background POSIX thread with secondary `SQLITE_OPEN_NOMUTEX` connection offloading `sqlite3_wal_checkpoint_v2` with zero loop pauses |
| **Authentication** | [`zerojwt/v2`](https://github.com/KaikeGM/zerojwt) zero-allocation HS256 JWT validator (`0 B/op`, `0 allocs/op`, ~640 ns/op, Rule 5 compliant) |
| **Transactions** | WAL Group Commit: batches all writes within an `epoll_wait` cycle into a single `BEGIN IMMEDIATE ... COMMIT` transaction (Rule 6 compliant) |
| **Nginx or Direct** | **Direct on `:80`**: zero reverse-proxy overhead, minimal user-space memory footprint per idle connection |

All Go module dependencies are pinned in `go.mod` and `go.sum`. The SQLite 3.53.4 amalgamation is pinned and SHA-256 verified in `build.sh`.

## Running it

```bash
sudo bash install.sh   # installs build-essential and Go toolchain
bash build.sh          # downloads pinned SQLite amalgamation, trains PGO profile and builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Key Architectural Optimizations

1. **Bare-Metal `epoll_wait` Event Loop Locked to OS Thread**
   Bypasses all high-level networking abstractions (`net/http`, `gnet`, etc.). Sockets are created with `SOCK_NONBLOCK | SOCK_CLOEXEC` via direct `accept4` syscalls. Connections are maintained in a flat array indexed directly by file descriptor (`conns []conn`), consuming zero heap allocations while idle.

2. **1-CGO-Call Batched Execution & Direct `Vdbe`/`Mem` Access**
   All requests ready in an `epoll_wait` cycle are parsed in user space and executed in **a single CGO call** (`C.db_exec_batch`). In-tree amalgamation sharing allows direct access to SQLite's internal virtual machine registers (`((Vdbe *)s)->pResultRow[i]` and `((Vdbe *)s)->aVar[i-1]`), completely bypassing SQLite function call overhead.

3. **3-Range-Scan `/feed` Algorithm**
   Replaces the 20 correlated scalar subqueries (`SELECT count(*) FROM likes WHERE post_id = p.id`) with 3 contiguous range scans:
   - Reads the top 20 IDs from the `(created_at, id)` index.
   - When IDs occupy a narrow window (`max_id - min_id < 256`), executes a single contiguous range scan on `likes_post_id_idx` (`SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2`), accumulating counts directly into a stack array (`uint32_t likes[256]`).
   - Scans posts in `[min_id, max_id]` and serializes output directly into the HTTP response arena.

4. **WAL Group Commit (Rule 6 Compliant)**
   Batches containing write operations (`POST /posts` and `POST /posts/:id/like`) share a single `BEGIN IMMEDIATE ... COMMIT` transaction. All writes are committed to the WAL before any HTTP response is returned to client sockets. If a commit fails, the transaction is rolled back and all operations in the batch return `500 Internal Server Error`.

5. **Zero-Allocation HS256 JWT Verification with [`zerojwt/v2`](https://github.com/KaikeGM/zerojwt)**
   Verifies every incoming bearer token signature and payload from scratch (`0 B/op`, `0 allocs/op`) via `VerifyWithScratch` bound to the event loop thread, ensuring strict compliance with Rule 5.

6. **In-Place HTTP Framing & SSE2 Vectorized JSON Escaping**
   Writes JSON response payloads at offset `pos + 128` in the output arena, computes the exact `Content-Length`, and prepends HTTP/1.1 response headers backwards directly in front of the body, eliminating intermediate buffers and extra memory copies.

7. **Asynchronous WAL Checkpointing in Dedicated C Pthread**
   Offloads `sqlite3_wal_checkpoint_v2` to a separate background thread with a secondary `SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX` connection. The main event loop triggers checkpoints via condition variables in ~50 ns without ever stalling for disk `fdatasync()` I/O.

8. **SQLite Profile-Guided Optimization (GCC PGO)**
   The in-tree SQLite C engine is compiled with profile-guided optimization (`-fprofile-generate`/`-fprofile-use`) trained during `build.sh` on an ephemeral scratch database against the challenge schema.

## Verification & Benchmarks

- **Official Functional Test Suite (`test/run.sh`):** **42/42 checks pass** (100% correctness).
- **High-Throughput Stress Test (`loadgen_pro.go`, 2,000 Concurrent VUs, 5–20 ms jitter, full 100:100:15:2 mix):**

| Concurrent VUs | Avg Throughput | Total Requests | Error Rate | Latency `p95` | Latency `p99` | Verdict |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **2,000 VUs** | **83,942 req/s** | **928,265** | **0.000%** | **20.0 ms** | **50.0 ms** | **PASS (0 Errors)** |
