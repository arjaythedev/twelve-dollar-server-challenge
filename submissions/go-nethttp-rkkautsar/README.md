# Go net/http + SQLite

**Language:** Go 1.27.1
**Framework:** standard library `net/http`
**Driver:** `github.com/mattn/go-sqlite3` v1.14.48 (cgo)
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx.

Five endpoints, one SQL round-trip per request, no response or query-result caching: every
request reads its data from SQLite while it is being served.

## Implementation notes

- `database/sql` pool of 8 connections, each opened with `_busy_timeout=5000`,
  `_journal_mode=WAL`, `_synchronous=NORMAL`, `_cache_size=-16384` (16 MiB per connection) and
  `_mmap_size=268435456` (256 MiB). The cache and mmap are SQLite's own page cache, which the
  rules allow; writes are committed before the 201 is sent.
- All five queries are prepared once at startup and reused (`*sql.Stmt`). Prepared statements
  cache the query plan, not row data, and are explicitly allowed by the rules.
- `like_count` is counted from `likes` in the request itself with a correlated subquery; no
  counter table or trigger is used, and the schema is unchanged.
- JWT HS256 verification is implemented directly with `crypto/hmac`; only `HS256` is accepted
  and `exp` is checked on every request (no token cache).
- Responses are hand-encoded into pooled byte buffers, with fixed key order and JSON string
  escaping (`"`, `\`, control bytes; non-ASCII is emitted as UTF-8). `Content-Length` is set
  and the buffer is returned to the pool after the write.
- Keep-alive is Go's default; no idle timeout is set, so idle sockets stay open far beyond the
  65 s Nginx requires when running behind it.

## Local measurements

`dev/loadtest.sh` runs both the server and k6 inside Docker Desktop's VM, with the server
capped at 1 CPU, 2 GB, no swap and 65,535 open files. A 5-minute hold at 10,000 VUs gave
0 failed requests, 933 req/s, p95 0.85 ms and p99 1.9 ms, using ~253 µs of CPU per request.
This is a local emulation, not the official benchmark.

## Scripts

- `install.sh` — Go 1.27.1 toolchain and `build-essential` (cgo).
- `build.sh` — pinned module download, verify, and a trimpath stripped build to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
- `dev/loadtest.sh` — optional local load rig (see `dev/README.md`).
