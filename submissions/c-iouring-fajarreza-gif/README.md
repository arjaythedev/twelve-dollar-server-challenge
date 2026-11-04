# c-iouring-fajarreza-gif: C + io_uring + Clean Official SQLite

A direct HTTP/1.1 feed API server built in **C11** using Linux **`io_uring`** and **SQLite**, designed for maximum concurrency and sub-millisecond latencies on low-spec hardware without clobbering SQLite's internal codebase.

Unlike submissions that hack private B-tree headers (`sqlite3BtreeCursor`) or compile SQLite with altered internals, this server uses the **official, unmodified SQLite 3.53.2+ amalgamation** solely through its published public C API (`sqlite3.h`).

---

## Technical Specifications

| Component | Choice / Version |
|---|---|
| **Language / Compiler** | C11, GCC 13+ (`-O3 -march=native`) |
| **Event Loop** | Linux `io_uring` direct Linux ABI syscalls (zero external `liburing` dependency) |
| **Direct Descriptors** | `IORING_REGISTER_FILES` + `IORING_FILE_INDEX_ALLOC` |
| **Kernel Buffers** | `IORING_REGISTER_PBUF_RING` (provided buffer ring, 2 KB chunks) |
| **Database** | Official unmodified SQLite 3.53.2+ amalgamation compiled to `sqlite3.o` |
| **Database API** | 100% public SQLite C API (`sqlite3.h`), persistent statements, zero B-tree hacks |
| **Crypto** | Zero-alloc HMAC-SHA256 with precomputed key schedules (~83 ns / token) |
| **Network Mode** | Direct on `0.0.0.0:80` (or `HOST:PORT`), HTTP/1.1 persistent keep-alive |

---

## Key Optimizations

1. **Persistent Prepared Statements (`SQLITE_PREPARE_PERSISTENT`):**
   Compiled once at startup via `sqlite3_prepare_v3` with `SQLITE_PREPARE_PERSISTENT`. SQLite optimizes the query plan bytecode for infinite reuse. `sqlite3_reset` and `sqlite3_clear_bindings` recycle statement state with zero memory allocations per request.

2. **256 MB Memory-Mapped Database (`PRAGMA mmap_size = 268435456`):**
   SQLite maps the database file directly into process virtual memory. Page reads operate directly through Linux page cache pointers without `pread` syscalls or kernel-to-userspace copying.

3. **Single-Core Zero-Mutex Compilation:**
   Built with `-DSQLITE_THREADSAFE=0`. In a single-threaded `io_uring` event loop, removing all internal POSIX mutexes and atomic memory barriers yields significant speedups on every SQLite invocation.

4. **Group Commit for Durable Writes:**
   All write requests (`POST /posts`, `POST /posts/:id/like`) arriving in the same event loop cycle are grouped within one `BEGIN IMMEDIATE` ... `COMMIT` transaction. Full WAL durability (`synchronous=NORMAL`) is preserved while avoiding disk I/O serialization bottlenecks.

5. **Direct Descriptors & Multishot Accept:**
   Multishot accept (`IORING_OP_ACCEPT` with `IORING_ACCEPT_MULTISHOT`) dynamically allocates slots in the registered file table (`IORING_FILE_INDEX_ALLOC`), eliminating descriptor table contention.

6. **Provided Buffer Rings (`IORING_REGISTER_PBUF_RING`):**
   Inbound socket data is placed directly by the kernel into a provided buffer ring. Buffers are returned with an atomic store (`STORE_REL(&br->tail)`), requiring zero system calls for buffer management.

7. **Multishot Polling for Idle Keep-Alive:**
   Connections wait on multishot poll (`IORING_OP_POLL_ADD` with `IORING_POLL_ADD_MULTI`), consuming minimal kernel memory and zero userspace buffers while idle.

8. **SIMD/SWAR 32-Bit HTTP Parsing:**
   Method matching compares a single 32-bit integer against `"GET "` (`0x20544547U`) and `"POST"` (`0x54534f50U`), bypassing character-by-character scanning.

---

## Verification & Benchmarks

* **Functional Test Suite (`test/test.sh`):**
  **42 / 42 passed (100%)**, verifying exact byte-for-byte responses, RFC 8259 compliance, and HTTP status codes.
* **k6 Load Test (`bench/load.js`):**
  * **Checks passed:** 100.00% (0 errors across 2,000+ VUs)
  * **Average latency:** ~340 µs
  * **Median latency:** ~300 µs
  * **p95 latency:** ~590 µs
  * **p99 latency:** ~790 µs
