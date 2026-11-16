# Rust + axum + SQLite (baseline)

A deliberately plain baseline, and the Rust counterpart of `go-nethttp-DblD`. Both have the same database layout, so the gap between their scores reflects the language and framework, not the design.

| | |
|---|---|
| Language | Rust 1.99.1 (official standalone installer, sha256-checked in `install.sh`) |
| HTTP | axum 0.8.9 on hyper, tokio multi-threaded runtime |
| SQLite driver | rusqlite 0.40.2 (`bundled` SQLite) |
| Front | **Nginx** (`bench/nginx.conf`, `127.0.0.1:3000`) |

## Design

- **One writer, a few readers.** All writes go through one connection behind a mutex. SQLite allows one writer at a time, and a single connection serializes writes in-process instead of retrying on `SQLITE_BUSY`. Reads use 4 connections, picked round-robin.
- **SQLite calls run on tokio's blocking pool** (`spawn_blocking`), so they don't stall the async workers.
- **WAL with `synchronous=NORMAL`**, the minimum rule 6 allows. Each write commits before the response is sent. There's no group commit yet.
- **Prepared statements** come from rusqlite's statement cache. It caches query plans, not data.
- **No caching** of any kind. Every request queries SQLite, and every token's HS256 signature and `exp` are verified (`hmac` + `sha2`, constant-time compare).
- hyper keeps idle keep-alive connections open until the client closes them, which satisfies the spec's Nginx requirement (at least 65 s).

## Optimizations

None yet, on purpose. Later PRs will each change one thing and measure it.
