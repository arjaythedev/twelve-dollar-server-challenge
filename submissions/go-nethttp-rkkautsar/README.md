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
  10 ms). Every response waits for the commit.
- **Four read connections** (`READ_CONNS`), prepared statements, WAL,
  `synchronous=NORMAL`, 16 MiB page cache, 256 MiB mmap, `_mutex=no`, and
  WAL autocheckpoint at 10,000 pages. Connections use a 1 s idle timeout
  (`IDLE_TIMEOUT_MS`); shorter timeouts caused `ECONNRESET` on create requests
  from clients reusing a socket the server had just closed.
- `Content-Length` is set; responses are not chunked.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | SQLite database |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `IDLE_TIMEOUT_MS` | `1000` | Keep-alive idle timeout |
| `WRITE_BATCH_MS` | `10` | Write group-commit window |
| `READ_CONNS` | `4` | SQLite read connections |
| `READ_COALESCE` | `1` | In-flight feed/post read sharing |
| `READ_BATCH_US` | `500` | Read coalescing window |
| `POST_BATCH` | `0` | Sorted/cached post-ID batching |

## Local measurements

Docker load test with the server pinned to one CPU and `1.5 GiB` of memory,
four LG clients (20 s ramp, 60 s hold), fresh seed database, tuned defaults:

| Users | Client results | Server |
|---:|---|---|
| 70,000 | p50 160–161 ms, p95 **709–714 ms**, p99 1.07–1.08 s, 0 errors | 94% CPU, 324 MiB RSS |

The 70k result is still above the 500 ms p95 target. The 1 s idle timeout avoids
create-request resets; shorter timeouts dropped p95 in some runs but produced
`ECONNRESET` failures from stale client connections. `POST_BATCH=0` remains the
default; the optional path deduplicates, sorts, and caches one prepared `IN`
statement per batch size.

The official service has `LimitNOFILE=65535`, so a net/http implementation is
capped at roughly 65k accepted keep-alive sockets. The 1 s idle timeout costs
memory but stays well below the 2 GiB limit; the stock per-connection buffers
are still released between VU think periods.

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
