# C / raw epoll / SQLite — benclawbot

**Language / framework:** GNU C11, no HTTP framework; native single-threaded epoll event loop.
**Database driver:** SQLite native C API, in-process, with live SQL reads.
**Networking:** Direct HTTP/1.1 on HOST:PORT, **no Nginx**.
**Auth / JSON:** OpenSSL HMAC-SHA256 and json-c for parsing; low-allocation direct JSON output for reads.

## Installation / build

Ubuntu 24.04 x86_64, 1 vCPU, 2 GB RAM, no swap; run from the challenge repository root:

```sh
sudo bash submissions/c-epoll-benclawbot/install.sh
bash test/run.sh submissions/c-epoll-benclawbot
```

install.sh pins Ubuntu 24.04 package versions for build-essential (12.10ubuntu1), pkg-config (1.8.1-2build1), libsqlite3-dev (3.45.1-1ubuntu2.8), libjson-c-dev (0.17-1build1) and libssl-dev (3.0.13-0ubuntu3.16); their transitive system packages resolve from the matching Ubuntu 24.04 repositories. build.sh compiles the included C source locally with system GCC; start.sh execs the binary in the foreground. No binary artifacts are committed.

The server configures itself from the standard SQLITE_PATH, JWT_SECRET, HOST and PORT environment variables. It does not change sysctls, require CPU affinity, need root, or modify the schema.

## Design and rule compliance

- Single-threaded, direct edge-triggered epoll with lightweight per-connection state. HTTP/1.1 keep-alive and backpressure; no application data caches.
- Each read executes a new SQLite query. Reused prepared statements store query plans only. The SQLite page cache is 8 MiB by default; WAL with synchronous=NORMAL and exclusive SQLite locking.
- Fresh /feed uses the existing (created_at,id) index and grouped likes_post_id_idx scans when an exact order-preserving dense range is available; otherwise it executes the reference feed SQL. No indices, triggers, or tables are added.
- Successful writes and grouped transactions finish committing before the response is sent. Authentication verifies each JWT HS256 signature and expiration anew on every request.
- Dynamic writes, exact error-message JSON, UTC timestamps, preserved JSON key ordering, UTF-8 and escaping. No hardcoded challenge responses or special benchmark-mode logic.
- Additional optional DB_CACHE_KIB, DB_MMAP_MIB, GROUP_COMMIT, SOCKET_BUF_KIB and FEED_IMPL environment controls exist for diagnostics only; defaults require only the four official variables.

## Validation and reproducible evidence

- **Official organizer test script:** test/test.sh, 42/42, passed=42 failed=0 on a fresh copy of the official generated database, on local Debian 13 x86_64 with SQLite 3.46.1 (October 13, 2026).
- **Additional independent local checks:** 39/39, including create, like, auth, round trips; sanitizers passed on previous local version.
- **Local native-client, CPU-saturated synthetic mixed workload:** around 45,282 requests/sec at 64 active clients; ~21.76 microseconds CPU/request, 0 HTTP failures. At 256 clients, approximately 44,387 requests/sec, ~22.20 microseconds CPU/request. These short tests do **not** reproduce the official bench/load.js mix, think times, five-minute holds or VM.
- **Not yet tested:** Ubuntu 24.04 clean-image install.sh + build.sh (upstream CI is authoritative), 64,000 simultaneous connections on the benchmark VM, and the official five-minute k6 capacity score. No claim to outperform the winning Rust result is made.

The earlier experimental one-shot io_uring backend was slower and used more memory in local paired tests, so it is not included in this submission. Those results do not establish that epoll beats the high-performing **multishot registered-file** Rust io_uring implementation; the architectures differ materially.

## License

MIT, under the challenge repository's MIT license.
