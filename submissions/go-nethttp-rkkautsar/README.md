# Go net/http + database/sql + SQLite

A conventional implementation: standard `net/http`, standard `database/sql`,
standard `encoding/json`/`crypto` and the stock `mattn/go-sqlite3` driver.
No custom HTTP parser, connection loop, response writer, or direct SQLite
binding. The SQLite driver is the only external dependency; Go's standard
library has no SQLite implementation.

Serving is direct on `HOST:PORT`, no Nginx.

## What we added over the stock stack

- **SQLite renders the JSON.** `/feed` and `/posts/:id` return one `TEXT` value
  built by `json_group_array(json_object(...))` / `json_object(...)`, not rows
  mapped and re-encoded in Go.
- **Optional in-flight read batching.** Enabled by default. Concurrent
  requests for the same feed/post join one SQLite read; the leader waits
  `READ_BATCH_US` (default 500 µs) so other handlers can join, and the group is
  deleted as soon as the read completes. A generation counter in the key is
  bumped after every commit, so a request that starts after a 201/200 cannot
  join a pre-write read. No result outlives its group. `READ_COALESCE=0`
  disables it.
- **Write group commit.** One dedicated `*sql.Conn` runs queued create/like
  jobs in one `BEGIN IMMEDIATE … COMMIT` every `WRITE_BATCH_MS` (default
  50 ms). Every response waits for the commit.
- **Four read connections** (`READ_CONNS`), prepared statements, WAL,
  `synchronous=NORMAL`, 16 MiB page cache, 256 MiB mmap, `_mutex=no`, and
  WAL autocheckpoint at 10,000 pages. Connections use a 10 ms idle timeout
  (`IDLE_TIMEOUT_MS`) so the stock 4 KiB-per-connection HTTP buffers are
  released between user requests; this is the reason memory stays low at 55k.
- `Content-Length` is set; responses are not chunked.

## Local measurements

Docker load test with the server pinned to one CPU and `1.5 GiB` of memory,
four LG clients (20 s ramp, 60 s hold), fresh seed database, tuned defaults:

| Users | Client results | Server |
|---:|---|---|
| 55,000 | p50 1.5–1.6 ms, p95 **505–520 ms**, p99 689–1,000 ms, 0.003–0.006% errors | 84% CPU, 40.7 MiB RSS |

The 55k result is close but still above the 500 ms p95 target. The remaining
tail is in the write path: the CPU profile showed `likePost`/`createPost`
goroutines waiting for the write batch, not SQLite reads or memory pressure.
`POST_BATCH=0` remains the default. The batched post-ID path now deduplicates,
sorts, and caches one prepared `IN` statement per batch size; it is still
opt-in pending official results.

The official service has `LimitNOFILE=65535`, so a net/http implementation is
capped at roughly 65k accepted keep-alive sockets. The 10 ms idle timeout keeps
the stock per-connection buffers from accumulating; it is a deliberate
throughput/memory tradeoff.

`test/run.sh` passes **42/42** with the default configuration.

## Prior art

- SQLite-rendered JSON and prepared statements are common; e.g.
  `go-nethttp-fi5HiE` uses the same `json_object`/`json_group_array` approach.
- Group commit is common across the leaderboard.
- In-flight feed coalescing was independently implemented in the top-ranked
  `c-iouring-meros` submission, but left **off by default** because its author
  judged rule 5 (“no response/query-result caches”) ambiguous for one query
  serving many requests. This build enables it by default; `READ_COALESCE=0`
  disables it.
- The custom branch’s prebuilt packet and single-write response are also not
  novel: top io_uring implementations write headers in front of the body in one
  send.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` for the cgo driver.
- `build.sh` — module download/verify and a stripped build to `bin/server`.
- `start.sh` — foreground server.
