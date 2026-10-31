# c-epoll-patrickryankenneth

- **Language / framework:** C (gcc, `-O3 -march=x86-64-v2`), no framework. Single-threaded `epoll` loop.
- **Database:** SQLite 3.50.4 amalgamation, built from source in `build.sh`, in-process, `SQLITE_THREADSAFE=0`.
- **Nginx:** none. Serves directly on `HOST:PORT` (set `0.0.0.0:80` for the benchmark).

## What it does
- Hand-rolled HTTP/1.1 parser with keep-alive and pipelining; idle connections are never timed out.
- Idle connections own no buffers: one shared read buffer, per-connection memory only for partial requests / blocked writes.
- Hand-rolled SHA-256 + HMAC with the ipad/opad midstates precomputed; every token's signature and `exp` is verified on every request.
- Validating, zero-allocation JSON scanner for tokens and request bodies.
- Prepared, persistent statements; WAL + `synchronous=NORMAL`; each write is committed before its response is sent.
- No response caches, query caches or table copies.
