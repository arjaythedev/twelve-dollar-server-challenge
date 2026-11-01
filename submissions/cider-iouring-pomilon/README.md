# cider-iouring-pomilon

Feed API submission implemented in **Cider** ([github.com/Pomilon/Cider](https://github.com/Pomilon/Cider)), an ahead-of-time compiled, freestanding systems programming language.

- **Language:** Cider v4 (native x86-64 code generator)
- **Runtime:** Single-threaded `io_uring` direct-descriptor engine
- **Direct or Nginx:** Direct (`HOST:PORT` HTTP/1.1 persistent keep-alive)
- **Database:** SQLite (in-process, standard SQL prepared statements, WAL mode with `synchronous=NORMAL`)
- **Purity:** 100% pure freestanding Cider application code (`src/server.cid`), with zero C application wrappers. Dynamically links system `libcrypto` (for HMAC-SHA256), `libpthread`, and `libc`, and links the bundled in-process SQLite amalgamation.

---

## What was done

1. **io_uring Fixed Direct Descriptors:**
   Uses `IORING_REGISTER_FILES` (up to 524,280 direct file slots) and `IORING_FILE_INDEX_ALLOC` on multishot accept, bypassing standard OS file descriptor limits and descriptor table contention.

2. **Kernel Provided Buffer Ring:**
   Pre-allocates an 8 MiB buffer ring (`IORING_REGISTER_PBUF_RING` with 4,096 buffers of 2 KB each) for incoming request packets, replenished directly in user space without syscalls.

3. **Zero-Malloc Response Buffer Pool:**
   Pre-allocated 4,096-slot pool of 4 KB buffers reused across request lifecycles to eliminate runtime heap allocation overhead.

4. **Group Commit & WAL Management:**
   Batches database writes across completion queue events within single transactions (`BEGIN` / `COMMIT`), ensuring responses are returned only after commits succeed. Uses pipelined 2,048-frame non-blocking passive checkpoints (`SQLITE_CHECKPOINT_PASSIVE`) scheduled after immediate socket submission to prevent event-loop stalls.

5. **Optimized SQLite Compilation:**
   Compiled with single-thread and no-mutex flags (`-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DHAVE_FDATASYNC=1`).

---

## Local Benchmark Results

Local stress test using a custom Go loopback generator (not the official k6 harness), with 15 loopback addresses and the server and load generator co-located:

- **Five-minute hold:** 165,000 VUs (passed)
- **Duration:** 300.21 s
- **Requests:** 5,177,479 total (5,177,476 passed, 3 failed)
- **Error Rate:** 0.00% (3 drops)
- **Throughput:** 17,246.4 req/s
- **Latency:** p50 1.05252 ms, p95 93.233287 ms, p99 157.420143 ms, max 603.729364 ms
- **cgroup Peak Memory:** 803 MB (`842,883,072` bytes)
- **Functional tests:** 42/42 passed (`test/run.sh`)

> *Note:* These are local results on a co-located loopback setup, not official scores. The custom generator is a local development tool, not part of this submission. The official challenge benchmark uses separate 16-core load generators over the datacenter network, so droplet results will differ. Run the local test on an otherwise idle host; co-located CPU-heavy processes materially change tail latency.

---

## Build and Run

```bash
# 1. Install toolchain & dependencies (Ubuntu 24.04, root)
# Clones and builds the Cider compiler from source into /opt/cider
sudo bash install.sh

# 2. Build server binary
bash build.sh

# 3. Run test suite
bash test/run.sh submissions/cider-iouring-pomilon

# 4. Start server in foreground
HOST=0.0.0.0 PORT=80 SQLITE_PATH=feed.db JWT_SECRET=twelve-dollar-challenge bash start.sh
```
