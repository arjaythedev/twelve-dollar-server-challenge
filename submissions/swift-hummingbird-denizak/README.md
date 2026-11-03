# swift-hummingbird-denizak

The $12 Server Challenge API implemented with **Swift 6.2 + Hummingbird 2.26** on top of
system **SQLite 3** through a thin C interop layer.

| | |
|---|---|
| Language | Swift 6.2.1 (pinned toolchain tarball from swift.org) |
| Framework | Hummingbird 2.26.0 (SwiftNIO-based HTTP/1.1 server) |
| SQLite driver | system `libsqlite3` via direct C interop (`CSQLite3` system-library target) |
| JWT | swift-crypto 3.12.3 (`HMAC<SHA256>`, constant-time compare) |
| **Nginx or direct** | **Direct** — the server listens on `0.0.0.0:80` (default when `HOST`/`PORT` are unset) |

## Why direct instead of Nginx

The Nginx config gzips every JSON response over 1 KB and adds a proxy hop; the video pegs
that overhead at 14–20% of the box's only core. Hummingbird/SwiftNIO handles the ~15,000
keep-alive connections of direct mode fine (epoll, a few KB of state per connection), so
the server serves the internet itself and keeps all of the CPU. Idle connections are never
closed (no idle timeout), so Nginx-style 65 s keep-alive requirements don't apply.

## Architecture

One process, two kinds of SQLite connections:

- **Readers** (4 by default, `READERS` env var): each guarded by its own lock, executed
  inline on the request's event-loop thread. In WAL mode readers never block the writer,
  and the round-robin pool spreads read queries over connections so one slow query never
  queues behind another. `GET /feed`, `GET /posts/:id` and the `SELECT 1` in `/health` run
  here with zero thread hops.
- **Writer** (1 connection): all writes go through a dedicated serial thread
  (`DispatchQueue`) and the response continuation resumes only after the statement has run
  to completion. Autocommit + `WAL` + `synchronous=NORMAL` (explicitly allowed by rule 6)
  means the row is in the WAL before `201` is produced, while WAL auto-checkpoint stalls
  land on the writer thread instead of stalling event loops.

Event-loop threads: 2 (`EL_THREADS` env var). One core does the work; the second hides
syscalls/checkpoint stalls.

## Optimizations

1. **Hand-rolled response serialization** straight into a pre-sized `ByteBuffer` — no
   `Codable`, no intermediate strings. Responses are built in one allocation; JSON
   escaping only escapes `"`, `\` and control chars (spec allows leaving non-ASCII and
   `<>&/` alone), so typical ASCII payloads hit a fast scan-and-copy loop.
2. **Prepared statements created once per connection at startup**, rebound per request.
   They cache the query plan only — every request still reads its rows from SQLite (rule 5).
3. **Zero-copy binds**: `sqlite3_bind_text` with `SQLITE_STATIC` while the Swift string /
   ByteBuffer backing the bytes is alive, avoiding a copy per write.
4. **`RETURNING id, created_at`** for post creation — the timestamp is produced by
   SQLite's own `strftime('%Y-%m-%dT%H:%M:%fZ','now')` default, so the server never
   formats time and can never disagree with the stored value.
5. **Constant-error responses** (`{"error":"not found"}` etc.) are built once at startup
   and shared; error paths are allocation-free.
6. **Hand-written JWT check** on every request (signature verification is mandatory per
   rule 5): base64url decode → alg must be `HS256` → `HMAC<SHA256>` with constant-time
   comparison → `exp`/`sub`/`username` validation. Auth happens before id validation on
   authenticated routes, per spec.
7. **Pragmas** (in-process settings, rule 11): `synchronous=NORMAL`, `busy_timeout=10000`,
   `cache_size=-8000` (8 MB/connection), `mmap_size=256 MB` (SQLite's own page cache /
   mmap — blessed by rule 5), `temp_store=MEMORY`.
8. **Strict positive-integer id parser**: ASCII digits only with overflow guard, so
   `0`, `-1`, `abc`, `1.5` (and 20-digit ids) are rejected without exceptions.

## Layout

```
Sources/CSQLite3/     system-library shim for <sqlite3.h>
Sources/App/
  main.swift          env config, routes, handlers
  Database.swift      connection setup, prepared statements, query execution
  JSONWriter.swift    ByteBuffer JSON string writer
  JWT.swift           HS256 verification
  Responses.swift     exact error bodies
  ErrorMapperMiddleware.swift  maps thrown errors to spec bodies (incl. router 404s)
```

## Reproduce

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/swift-hummingbird-denizak   # 42/42 checks green
k6 run -e VUS=2000 bench/load.js                         # optional load test
```

Dependencies are pinned in `Package.resolved` (every pin has a
`swift-tools-version <= 6.2`, matching the installed 6.2.1 toolchain).
