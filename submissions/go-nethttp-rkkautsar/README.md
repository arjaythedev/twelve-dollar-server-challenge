# Go net/http + SQLite

**Language:** Go 1.27.1
**Framework:** standard library `net/http` (request parser), custom connection loop
**Driver:** `github.com/mattn/go-sqlite3` v1.14.48 (cgo)
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx.

Five endpoints, one SQL round-trip per request, no response or query-result caching: every
request reads its data from SQLite while it is being served.

## Implementation notes

- **Split read/write paths.** Reads use a 4-connection pool. Writes use one dedicated
  connection and a writer goroutine that group-commits queued create/like jobs every
  `WRITE_BATCH_MS` (default 100 ms). Every write response waits for that commit, so a 201/200
  is only sent for durable rows. Rule 6 explicitly allows group commit under that condition.
- Each connection is opened with `_busy_timeout=5000`, `_journal_mode=WAL`,
  `_synchronous=NORMAL`, `_cache_size=-16384` (16 MiB) and `_mmap_size=268435456` (256 MiB).
  The cache and mmap are SQLite's own page cache, which the rules allow.
- All five queries are prepared once at startup and reused. Prepared statements cache the
  query plan, not row data.
- `like_count` is counted from `likes` in the request itself with a correlated subquery; no
  counter table or trigger is used, and the schema is unchanged.
- JWT HS256 verification is implemented directly with `crypto/hmac`; only `HS256` is accepted
  and `exp` is checked on every request (no token cache). Claims are decoded into a struct.
- Responses are hand-encoded into pooled byte buffers with fixed key order and JSON string
  escaping. The pooled buffer is transferred to the connection writer (no copy) and returned
  to the pool after the response is flushed. The connection loop is built on
  `http.ReadRequest` with a 2 KiB read buffer and a 6 KiB `bufio.Writer`: the stock
  `net/http` server hardcodes a 4 KiB write buffer, so the 5,102-byte feed would otherwise
  need two write syscalls. One `Write` per response; `TCP_NODELAY` is set explicitly.
- Keep-alive is the HTTP/1.1 default; idle connections are not closed.
- `PPROF_ADDR` optionally starts a `net/http/pprof` listener; it is unset in benchmark runs.

## Local measurements

`dev/loadtest.sh` runs both the server and k6 inside Docker Desktop's VM, with the server
capped at 1 CPU, 2 GB, no swap and 65,535 open files. 10,000 VUs for 5 minutes:

| Metric | Value |
|---|---:|
| Requests | 373,320 |
| Throughput | 933 req/s |
| p95 / p99 | 42.6 ms / 100.6 ms (100 ms write window) |
| Failed requests | 0 |
| CPU per request | **162.8 µs** |
| Peak server memory | 340 MB |

CPU/request evolution on the same 10k run: 253 µs baseline → 211 µs with the batched writer
→ 167.5 µs with the connection loop → **162.8 µs** after transferring body buffers and
shrinking per-connection buffers (6 KiB write + 2 KiB read). Allocation churn fell from
224 MB to 89 MB per 30 s and live heap from 126 MB to 96 MB; peak RSS dropped 405 → 340 MB.

These are local emulations, not the official benchmark. Above ~10k VUs the rig is limited by
Docker Desktop's VM memory (k6 VU runtimes), not by the server.

## Scripts

- `install.sh` — Go 1.27.1 toolchain and `build-essential` (cgo).
- `build.sh` — pinned module download, verify, and a trimpath stripped build to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
- `dev/loadtest.sh` — optional local load rig (see `dev/README.md`).
