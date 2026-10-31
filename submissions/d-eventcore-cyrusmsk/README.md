# d-eventcore-cyrusmsk

A single-threaded Linux eventcore feed server written in D. It serves the
internet directly on `0.0.0.0:80`, with no Nginx in front.

- `eventcore` epoll driver for accept/read/write readiness, with one event-loop
  batch followed by response writes and a shared commit where needed.
- In-tree SQLite amalgamation compiled with `SQLITE_THREADSAFE=0`, WAL
  synchronous=NORMAL, a 64 MiB page cache and a 256 MiB mmap.
- Prepared statements for every endpoint. Writes share one `BEGIN IMMEDIATE`
  transaction per event batch, and responses are submitted after commit.
- Two-pass profile-guided optimization: SQLite C (`gcc`) and D (`ldc2`) are
  instrumented, trained with `bin/server --pgo-train`, then rebuilt with the
  collected profiles and LTO. Set `PGO=0` to skip PGO.
- A hand-written JSON writer with a 16-byte SIMD fast path, plus a strict
  single-pass parser for JWT payloads and request bodies.
- HS256 verification and a small SHA-256/HMAC implementation avoid per-request
  JWT allocations.

Build with `bash build.sh`; run with `SQLITE_PATH=... JWT_SECRET=... bash start.sh`.
