# Rust + Axum + SQLite

A Rust implementation of the five-endpoint feed API.

**Direct HTTP/1.1 with keep-alive, without Nginx.** The server binds `HOST:PORT` from the
environment (default `0.0.0.0:80`), so it serves the internet itself. `start.sh` raises the
soft descriptor limit to the inherited hard limit (65535 on the benchmark box).

## Stack and pinned versions

- Rust (edition 2024, 1.85+)
- Axum 0.7.9 (Hyper 1.12 / Tokio 1.54)
- `rusqlite` 0.43.0 with the bundled SQLite
- `serde` 1.0.229 / `serde_json` 1.0.154
- `base64` 0.22.1, `hmac` 0.12.1, `sha2` 0.10.8

`Cargo.lock` pins the full dependency resolution.

## Build and run

```sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Database behavior

Every request reads or writes SQLite directly; there are no response, query, or JWT caches.
Prepared statements are reused (they cache the plan, not data). WAL with `synchronous=NORMAL`,
foreign keys on, and a busy timeout are configured per connection. `POST /posts` lets SQLite's
column default produce `created_at` and returns the stored value via `RETURNING`. A like that
hits a foreign-key violation reports `post not found`; transient `SQLITE_BUSY`/`SQLITE_LOCKED`
errors are retried rather than misreported as 404s.

Every authenticated request verifies the HS256 signature and `exp`; no verification results are
remembered.

## Optimizations in this revision (Phase 0)

- **Direct serving** (no Nginx), TCP_NODELAY on accepted sockets.
- **Fat LTO**, one codegen unit, `panic=abort`, stripped symbols, `-C target-cpu=native`.
- **Typed JSON responses** instead of `serde_json::Value` trees, so each response is serialized
  once and field order is preserved by struct layout (no `preserve_order` feature).
- **SQLite-generated timestamps** for creates, removing a client-side time formatter.
- **Correct error handling**: spec-exact `internal server error` bodies and a health endpoint
  that actually runs `SELECT 1` and reports 503 when the database is unreachable.
- **Spec-exact JWT claim validation**: `sub` must be a positive integer string, `username` any
  string.

## Planned next phases

1. Single database actor with batching (replace per-request `spawn_blocking`), manual byte-buffer
   JSON output, cheaper JWT decoding, SQLite cache/checkpoint tuning.
2. SQLite B-tree cursor reads for the feed and post lookups, group commit for writes.
3. Transport swap (epoll/io_uring) behind the existing request/response boundary.
