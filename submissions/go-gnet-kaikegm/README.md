# Go + gnet (`go-gnet-kaikegm`)

High-performance, single-threaded Go feed API built for **The $12 Server Challenge**, sustaining **250,000 concurrent virtual users (C250k) at `0.000%` error rate** (`10 ms` `p95`, `50 ms` `p99`, `191.41 MiB` RSS, `1,258,121` requests), **300,000 concurrent virtual users (C300k)** within the `<1.0%` threshold (`0.055%` error rate, `20 ms` `p95`, `50 ms` `p99`, `184.93 MiB` RSS, `1,586,894` requests), and **44,042 req/s (peaking at 49,643 req/s)** max throughput.

| | |
|---|---|
| **Language & Compiler** | Go 1.27.2 (built with Profile-Guided Optimization `default.pgo`, `-trimpath -ldflags="-s -w"`) |
| **Networking** | [`gnet`](https://github.com/panjf2000/gnet) v2.10.0 single-threaded `epoll` event loop (`LockOSThread(true)`, `TCP_NODELAY`) |
| **HTTP Engine** | Custom zero-allocation HTTP/1.1 keep-alive parser and response serializer on `:80` |
| **SQLite Driver** | In-tree CGO binding to pinned SQLite 3.53.4 amalgamation (`-O3 -march=native -flto -DNDEBUG`, `SQLITE_THREADSAFE=2`, `SQLITE_USE_ALLOCA=1`) |
| **JSON Serialization** | Direct C row-to-JSON serializer (`q_feed`, `q_post`) with vectorized span `memcpy` into Go response buffer in a single CGO call (~40% faster `/feed`) |
| **Authentication** | [`zerojwt/v2`](https://github.com/KaikeGM/zerojwt) zero-allocation HS256 JWT validator (`0 B/op`, `0 allocs/op`, ~640 ns/op, Rule 5 compliant) |
| **Nginx or Direct** | **Direct on `:80`**: handles **250,000–300,000 keep-alive connections** with **~185–191 MiB RAM**, saving 15–20% Nginx CPU overhead |

All Go module dependencies are pinned in `go.mod` and `go.sum`. The SQLite 3.53.4 amalgamation is pinned and SHA-256 verified in `build.sh`.

## Running it

```bash
sudo bash install.sh   # installs build-essential and Go 1.27.2 toolchain
bash build.sh          # downloads pinned SQLite 3.53.4 amalgamation and compiles bin/server with PGO + LTO
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Key Architectural Optimizations

1. **Single-Threaded `epoll` Event Loop (`gnet`) Locked to OS Thread (`:80` Direct, No Nginx)**
   Standard `net/http` spawns one goroutine per active TCP connection. Under 64,000 to 300,000 persistent keep-alive connections, goroutine stack overhead and scheduler context switches consume hundreds of megabytes and saturate a 1-vCPU core. `gnet` multiplexes all connections on a single non-blocking `epoll` loop locked to the OS thread (`GOMAXPROCS(1)`, `LockOSThread(true)`), bypassing Nginx to save 15–20% of the single vCPU.

2. **Direct C Row-to-JSON Serialization (`q_feed`, `q_post`) with Vectorized Span `memcpy` — Single CGO Crossing**
   Instead of scanning rows across CGO boundaries into Go structs or paying for SQLite's `json_object`/`group_concat` dynamic allocations and CTE materialization (`MATERIALIZE`), `/feed` and `/posts/:id` step index-ordered `CROSS JOIN` queries in C and serialize the exact JSON byte stream directly into the Go HTTP response buffer in a single CGO call (cutting `/feed` CPU time by ~40%). Clean ASCII spans in `body` and `author` are copied in bulk via `memcpy`.

3. **In-Tree Lock-Free SQLite 3.53.4 (`EXCLUSIVE` + 1 GiB `mmap` + WAL Page Cache)**
   Compiled in-tree with `-O3 -march=native -flto -DNDEBUG -fomit-frame-pointer -finline-functions -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_USE_ALLOCA=1 -DSQLITE_OMIT_PROGRESS_CALLBACK=1 -DSQLITE_OMIT_SHARED_CACHE=1`. Configured with `PRAGMA locking_mode=EXCLUSIVE` (zero `-shm` lock syscalls), `journal_mode=WAL`, `synchronous=NORMAL` (Rule 6), `mmap_size=1073741824` (1 GiB zero-copy memory-mapped reads), `cache_size=-16384` (keeping hot WAL index pages in RAM), and `wal_autocheckpoint=10000`.

4. **Zero-Allocation HS256 JWT Verification with [`zerojwt/v2`](https://github.com/KaikeGM/zerojwt) (Rule 5 Compliant)**
   Verifies every incoming token signature from scratch (`0 B/op`, `0 allocs/op`, **~640 ns/op**) using [`github.com/KaikeGM/zerojwt/v2`](https://github.com/KaikeGM/zerojwt) in `VerifyWithScratch` mode (single-threaded scratch buffer bound to the event loop, zero heap allocations, zero synchronization, compliant with Rule 5 zero-caching).

5. **Profile-Guided Optimization (`default.pgo`) & Runtime Tuning**
   Built with a CPU profile (`default.pgo`) captured under realistic load so the Go compiler optimizes hot-path inlining and branch layout (`OnTraffic`, `parseRequest`, `verifyJWT`, `dbFeed`), paired with `debug.SetGCPercent(400)` and `debug.SetMemoryLimit(1200 << 20)`.

## Benchmark Verification

### Test Hardware & Environment

All local benchmarks were executed on a single laptop where **both the server and the 300,000-VU load generator ran simultaneously on the same machine** over loopback with hardware core isolation (`sched_setaffinity`):

| Component | Specification |
| :--- | :--- |
| **Host Machine** | Lenovo IdeaPad 3 15ITL6 (82MD) |
| **CPU** | 11th Gen Intel Core i5-1135G7 @ 4.20 GHz (**Server pinned to Physical Core 0** via `GOMAXPROCS(1)` + `LockOSThread(true)` + `sched_setaffinity`) |
| **OS & Kernel** | CachyOS x86_64 — Linux `7.2.9-2-cachyos` |
| **Memory & Storage** | 7.54 GiB RAM (`btrfs` SSD) — **Server RSS peaked at 161–191 MiB** (`SetMemoryLimit(1200 MiB)`) |
| **Load Generator** | [`loadgen_pro.go`](./loadgen_pro.go) — Multi-IP raw-TCP Go load generator with zero-allocation persistent-FD `/proc` per-process telemetry |

- **Official Functional Test Suite (`test/run.sh`):** **42/42 checks pass** (100% correctness).
- **Official `k6` User Loop Simulation (`loadgen_pro.go`, 3–8 s / 5–15 s think time, 15% like, 2% post):**

| Concurrent VUs | Total Requests | Peak RPS (Avg) | Error Rate | Server RAM (`VmRSS`) | Latency `p50` | Latency `p95` *(<500ms)* | Latency `p99` *(<1000ms)* | Verdict |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **50,000 VUs** | 124,435 | 9,258 (2,643/s) | **0.000%** | **77.10 MiB** | **500 µs** | **500 µs** | **2 ms** *(Max: 9.5ms)* | PASS |
| **100,000 VUs** *(C100k)* | 396,538 | 14,500 (6,403/s) | **0.000%** | **84.00 MiB** | **500 µs** | **2 ms** | **10 ms** | PASS |
| **180,000 VUs** | 767,502 | 28,348 (10,107/s) | **0.000%** | **128.70 MiB** | **500 µs** | **10 ms** | **200 ms** | PASS |
| **200,000 VUs** *(C200k)* | 904,529 | 27,221 (11,290/s) | **0.000%** | **161.26 MiB** | **500 µs** | **5 ms** | **10 ms** *(Max: 24.5ms)* | **PASS (0 Errors, 61% CPU)** |
| **250,000 VUs** *(C250k Clean Peak)* | **1,258,121** | **34,393 (14,175/s)** | **0.000%** | **191.41 MiB** | **500 µs** | **10 ms** | **50 ms** *(Max: 106.3ms)* | **PASS (0 Errors, 70% CPU)** |
| **300,000 VUs** *(C300k Max Ceiling)* | **1,586,894** | **35,300 (8,125/s)** | **0.055%** | **184.93 MiB** | **1 ms** | **20 ms** | **50 ms** *(Max: 114.9ms)* | **PASS (<1.0% Error)** |

- **Per-Process `/proc` Telemetry Map at 250,000 Concurrent VUs (500,000 Open TCP Sockets):**

| Telemetry Metric (`/proc` Persistent FDs) | `server` Process | `loadgen_pro` Process |
| :--- | :--- | :--- |
| **CPU Core Affinity (`sched_setaffinity`)** | Physical Core 0 (`CPUs 0,4`) | Physical Cores 1–3 (`CPUs 1-3,5-7`) |
| **Total CPU Usage (Avg / Peak)** | **35.0% / 70.0%** | 50.6% / 150.0% |
| **User vs Kernel CPU (`utime` / `stime`)** | **19.4% User / 15.6% Kernel** | 20.1% User / 30.5% Kernel |
| **Peak Physical RAM (`VmRSS`)** | **191.41 MiB** (`163.5 MiB` Anon + `3.15 MiB` `mmap`) | **1,522.10 MiB** (`6.08 KB` per VU) |
| **Voluntary / Involuntary Context Switches** | `495,556` vol / **`786` preempt** | `2,793,228` vol / `10,707` preempt |
| **Logical Syscall Writes (`wchar` / `syscw`)** | `3,520.16 MiB` (`1,955,715` syscalls) | `101.08 MiB` (`1,307,017` syscalls) |
| **Kernel TCP Drops (`ListenDrops` / `TCPSynRetrans`)** | **`0` / `0`** | **`0` / `0`** |

- **Max Throughput Stress (`-fast=true`, 5–20 ms jitter, full request mix):**

| Concurrent VUs | Avg Throughput | Total Requests | Error Rate | Max RAM (RSS) | Latency `p50` |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1,000 VUs** | **35,577 req/s** | 536,135 | **0.000%** | 28.29 MiB | 13.7 ms |
| **2,000 VUs** | **44,042 req/s** | 884,203 | **0.000%** | 33.01 MiB | 50.0 ms |
| **5,000 VUs** | **33,616 req/s** | 510,846 | **0.000%** | 46.29 MiB | 131.3 ms |
| **25,000 VUs** | **30,609 req/s** | 484,826 | **0.000%** | 78.72 MiB | 760.0 ms |
| **80,000 VUs** | **20,399 req/s** | 375,585 | **0.000%** | 121.89 MiB | 3.52 s |

## Load Testing & Process Telemetry Guide (`loadgen_pro.go`)

Standard single-IP clients hit the Linux ephemeral port ceiling (~28k–64k sockets) and `net/http` goroutine/buffer overhead before saturating the server. The standalone load generator in [`loadgen_pro.go`](./loadgen_pro.go) (tagged with `//go:build ignore` so it does not interfere with `build.sh`):
- Binds across **25 loopback IPs (`127.0.0.1`–`127.0.0.25`)** using raw TCP sockets and reproduces the exact user flow from `bench/load.js`.
- Keeps VU stacks at Go's minimum **2 KB** (`~6 KB/VU` total RSS) via a 64-worker dialer pool, `sync.Pool` request I/O buffers, and an inline 8-byte `xorshift64` PRNG.
- Maps per-process telemetry (`server`, `loadgen_pro`, and Linux kernel `/proc/net/netstat`) at `<0.05%` CPU cost by keeping `/proc/<pid>/{stat,status,io}` file descriptors open and sampling them via zero-allocation `syscall.Pread` at offset `0`.
- Isolates hardware L1/L2 caches via `sched_setaffinity` (`-pin-cpus=true`), pinning `server` to Physical Core 0 and `loadgen_pro` to the remaining cores.

### 1. Build the Server and Load Generator

Run from the root of the repository:

```bash
# Build the server binary (bin/server)
bash submissions/go-gnet-kaikegm/build.sh

# Compile the standalone load generator
go build -o /tmp/loadgen_pro ./submissions/go-gnet-kaikegm/loadgen_pro.go
```

### 2. Start the Server on a Clean Copy of `seed/feed.db`

```bash
ulimit -n 1048576
rm -f /tmp/bench_feed.db*
cp seed/feed.db /tmp/bench_feed.db

JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 SQLITE_PATH=/tmp/bench_feed.db \
  ./submissions/go-gnet-kaikegm/bin/server &
SERVER_PID=$!
```

### 3. Run the Benchmark

- **Official `k6` User Loop Simulation (e.g., 250,000 Concurrent VUs with CSV Telemetry Export):**
  ```bash
  /tmp/loadgen_pro -users 250000 -rampup 50s -duration 10s -tokens seed/tokens.json -pid $SERVER_PID -csv /tmp/telemetry_250k.csv
  ```

- **Max Throughput Stress Mode (`-fast=true`, 5–20 ms jitter):**
  ```bash
  /tmp/loadgen_pro -users 1000 -rampup 5s -duration 15s -fast=true -tokens seed/tokens.json -pid $SERVER_PID
  ```

### Available CLI Flags (`loadgen_pro`)

| Flag | Default | Description |
| :--- | :--- | :--- |
| `-users` | `1000` | Number of concurrent virtual users (persistent TCP connections) |
| `-rampup` | `10s` | Gradual ramp-up window to establish all connections smoothly |
| `-duration` | `20s` | Steady-state measurement plateau after ramp-up |
| `-pid` | `0` | Server process PID to map `/proc/<pid>/{stat,status,io}` and kernel `/proc/net/netstat` |
| `-pin-cpus` | `true` | Isolate CPU cores via `sched_setaffinity` (Server on Core 0, Loadgen on remaining cores) |
| `-csv` | `""` | Optional file path to export second-by-second per-process telemetry as CSV |
| `-tokens` | `seed/tokens.json` | Path to the 20,000 JWT test tokens generated by `seed/make-seed.sh` |
| `-target` | `127.0.0.1:3000` | Target `host:port` of the running server |
| `-fast` | `false` | `false` = official `k6` think times (3–8 s / 5–15 s); `true` = max RPS stress mode (5–20 ms) |

## License

MIT, under the repo's [license](../../LICENSE).
