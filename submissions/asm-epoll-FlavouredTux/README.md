# x86_64 Assembly (NASM) Raw Linux Epoll Implementation

An ultra-high-performance bare-metal web server written directly in **pure x86_64 Assembly (NASM)** for the $12 Server Challenge.

Author: **[@FlavouredTux](https://github.com/FlavouredTux)**

---

## Stack & Architecture

- **Language:** Pure x86_64 Assembly (NASM ELF64, Intel syntax)
- **Networking:** Raw Linux `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`) with non-blocking sockets (`SOCK_NONBLOCK | SOCK_CLOEXEC`)
- **HTTP/1.1 Engine:** Custom zero-heap-allocation HTTP parser and response serializer with keep-alive support
- **Database Driver:** Direct C ABI binding to SQLite (`libsqlite3.so`)
- **Crypto:** OpenSSL `libcrypto.so` (`EVP_sha256()`, `HMAC`) with handwritten Base64URL encoder/decoder
- **Deployment Mode:** **Direct (`0.0.0.0:80`)** — We chose Direct mode to eliminate the 14–20% CPU overhead incurred by running Nginx on a 1 vCPU machine. The assembly server natively handles keep-alive connections and implements adaptive pressure-based socket eviction. It also seamlessly operates behind Nginx on `127.0.0.1:3000` via `HOST` / `PORT` environment variables.

---

## Key Optimizations & Design Choices

1. **Single-Threaded Event-Driven Concurrency on 1 vCPU:**
   - On a 1-core machine, thread context switching, cache ping-ponging, and mutex contention waste valuable CPU cycles.
   - A single-threaded Linux `epoll` event loop keeps CPU instructions executing in a tight loop with maximum L1 instruction/data cache residency.
   - Sockets are configured with `SO_REUSEADDR` and `SO_REUSEPORT`.

2. **Zero Dynamic Allocation (Zero `malloc`/`free` in request path):**
   - Direct array lookup `conn_table[fd]` with demand-paged memory (`mmap`).
   - Parsing, unescaping, validation, and JSON generation all operate directly on fixed per-connection and thread-local scratch buffers.
   - Zero garbage collection pauses, zero heap fragmentation.

3. **Optimized Zero-Syscall Accept Loop:**
   - Client connections are accepted with `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)` and added to epoll using a static stack slot, eliminating unnecessary intermediate `setsockopt` and stack frame adjustments to drain the kernel SYN/listen queue as fast as possible.
   - `MAX_EVENTS` is tuned to 4,096 to batch event collection in high-concurrency bursts.

4. **Adaptive Pressure-Based Keep-Alive Eviction:**
   - When active connection count reaches high load thresholds (`active_conns >= 15,000`), the server automatically emits `Connection: close` on idle connections to recycle file descriptors dynamically, avoiding OS fd exhaustion while maintaining high throughput.

5. **Pre-Prepared SQLite Statements:**
   - All SQL statements are prepared once at server startup (`sqlite3_prepare_v2`), eliminating query compilation overhead during requests.
   - In full compliance with Rules 2 & 6: writes are durable before response (`PRAGMA journal_mode = WAL`, `PRAGMA synchronous = NORMAL`).

6. **Hardware-Efficient Crypto & String Routines:**
   - Hand-rolled Base64URL encoder that serializes 32-byte HMAC digests directly into 43-byte signatures in CPU registers.
   - Full compliance with Rule 5: fresh HMAC-SHA256 signature verification and expiration check on every authenticated request with zero cross-request caching.
