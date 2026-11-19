# Rust + Axum + SQLite

A Rust implementation of the five-endpoint feed API.

**Direct HTTP/1.1 with keep-alive, without Nginx.** The server binds `HOST:PORT` from the
environment (default `0.0.0.0:80`), so it serves the internet itself. `start.sh` raises the
soft descriptor limit to the inherited hard limit (65535 on the benchmark box).

## Stack and pinned versions

- Rust 1.99.1 (edition 2024), installed system-wide under `/opt` by `install.sh` from the
  official standalone installer, sha256-verified (no rustup)
- Axum 0.7.9 (Hyper 1.12 / Tokio 1.54)
- `rusqlite` 0.43.0 with the bundled SQLite
- `serde` 1.0.229 / `serde_json` 1.0.154
- `base64` 0.22.1, `hmac` 0.12.1, `sha2` 0.10.8

`Cargo.lock` pins the full dependency resolution, and `build.sh` builds with `--locked`.

## Build and run

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` runs as root and installs the toolchain under `/opt/rust-1.99.1`, so `build.sh` can
run as an unprivileged user. It is idempotent and supports x86_64 and aarch64.

## Database behavior

Every request reads or writes SQLite directly; there are no response, query, or JWT caches.

One dedicated thread owns the SQLite connection and its prepared-statement cache. HTTP tasks send
commands through a bounded channel and await a oneshot reply, so database work never runs on the
async runtime. The actor drains every command already queued (up to 256), executes reads
immediately, and commits all of the batch's writes in one `BEGIN IMMEDIATE` transaction. No
success reply is sent before `COMMIT` returns successfully.

WAL with `synchronous=NORMAL`, foreign keys on, a 64 MiB page cache, and no mmap are configured
for that connection. Auto-checkpointing is relaxed to 10,000 pages and a separate low-priority
thread runs `wal_checkpoint(PASSIVE)` every 100 ms, keeping the WAL small without putting
checkpoint I/O in the request path. `POST /posts` lets SQLite's column default produce
`created_at` and returns the stored value via `RETURNING`. A like that hits a foreign-key
violation reports `post not found`; transient `SQLITE_BUSY`/`SQLITE_LOCKED` errors are not
misreported as 404s, and an unexpected statement error rolls back the batch and fails its writes.

Every authenticated request verifies the HS256 signature and `exp`; no verification results are
remembered.

## Optimizations

- **Direct serving** (no Nginx), TCP_NODELAY on accepted sockets.
- **Fat LTO**, one codegen unit, `panic=abort`, stripped symbols, `-C target-cpu=native`.
- **Single database actor with batching** and group-committed writes (Phase 1).
- **Hand-written compact JSON** into a reserved byte buffer, with field order fixed by the writer.
  Feed and post responses escape SQLite's borrowed row bytes directly, with no row or string
  copies. Each response is reserved once and no `serde_json::Value` tree is built.
- **Cheap JWT verification**: the HMAC key schedule is built once and cloned per request, header
  and payload base64 decode into stack buffers (heap only for oversized tokens), and the header
  borrows its algorithm string from that buffer. The signature is still checked on every request.
- **SQLite-generated timestamps** for creates, removing a client-side time formatter.
- **Correct error handling**: spec-exact `internal server error` bodies and a health endpoint
  that actually runs `SELECT 1` and reports 503 when the database is unreachable.
- **Spec-exact JWT claim validation**: `sub` must be a positive integer string, `username` any
  string.

## Planned next phases

1. SQLite B-tree cursor reads for the feed and post lookups, plus range like counts.
2. Transport swap (epoll/io_uring) behind the existing request/response boundary.

## Credits

This is an independent implementation, but two ideas follow earlier submissions in this
repository:

- The single-actor SQLite design — one connection owner, a bounded request queue with oneshot
  replies, batches of up to 256 commands, and one `BEGIN IMMEDIATE` transaction per batch — is
  modeled on [`rust-axum-tbsvttr`](../rust-axum-tbsvttr).
- `install.sh` and the matching `PATH` handling in `build.sh` are adapted from
  [`rust-axum-DblD`](../rust-axum-DblD) and [`rust-axum-connorw-dev`](../rust-axum-connorw-dev),
  which install a pinned, checksum-verified standalone Rust toolchain under `/opt` so that the
  unprivileged benchmark user can build.
