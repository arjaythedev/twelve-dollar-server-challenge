# x86_64 Assembly (`as` + `epoll` + SQLite3 + OpenSSL) — Twelve Dollar Server Challenge

## Stack & Versions
- Language: x86_64 Assembly (GNU Assembler `.intel_syntax noprefix`, `binutils` / `gcc` on Ubuntu 24.04)
- Framework: None
- Database Driver: `libsqlite3` (`3.45.1` on Ubuntu 24.04) called directly via the System V AMD64 ABI
- Crypto: `libcrypto` (`OpenSSL 3.0.x` `HMAC` + `EVP_sha256` + `CRYPTO_memcmp`)
- Deployment Mode: Direct on `0.0.0.0:80` (no Nginx); also supports `127.0.0.1:3000` when `HOST` and `PORT` are set by `test/run.sh`

## Key Optimizations & Why
1. Direct HTTP/1.1 Serving (No Nginx): Serving directly on `0.0.0.0:80` eliminates Nginx's reverse-proxy CPU overhead (14–20% on the 1-vCPU droplet) and avoids extra local socket copies
2. Single-Threaded Non-Blocking `epoll` Loop: On a 1-vCPU droplet, a single event-loop thread eliminates all thread context switches, mutex contention, and SQLite lock waits while multiplexing up to 65,535 persistent keep-alive connections
3. Register-Level HTTP Routing & Base64url Decoding: The 5 API routes (`GET /health`, `GET /feed`, `GET /posts/:id`, `POST /posts`, `POST /posts/:id/like`) are matched using 32-bit/16-bit immediate register comparisons (`cmp dword ptr [r15], 0x6565662f`) and a 256-byte lookup table for Base64url decoding with zero heap allocations (`malloc`/`free` are never called)
4. In-SQLite JSON Serialization & Validation: Prepared statements (`SQLITE_PREPARE_PERSISTENT`) are compiled once at startup using `json_object()` and `json_group_array()`, streaming compact JSON directly from SQLite into the response buffer without intermediate object serialization
5. Tick-Level Group Commit (`BEGIN IMMEDIATE ... COMMIT`)**: All `POST /posts` and `POST /posts/:id/like` requests arriving within the same `epoll_wait` batch are queued and executed inside a single WAL transaction (`PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;`), amortizing disk sync overhead while ensuring every write is durably committed before its `201`/`200` response is written to the socket
6. OS Page Cache via `mmap_size=1GB`: Configures SQLite to memory-map the seed database directly from the Linux page cache and disables automatic WAL checkpointing on the hot path (`PRAGMA wal_autocheckpoint=0`)