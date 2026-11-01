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
- **Optional in-flight read batching.** Disabled by default so every request
  executes its own SQLite read. With `READ_COALESCE=1`, concurrent requests for
  the same feed/post join one SQLite read; the leader waits `READ_BATCH_US`
  (default 500 µs) so other handlers can join, and the group is deleted as soon
  as the read completes. A generation counter in the key is bumped after every
  commit, so a request that starts after a 201/200 cannot join a pre-write read.
  No result outlives its group.
- **Write group commit.** One dedicated `*sql.Conn` runs queued create/like
  jobs in one `BEGIN IMMEDIATE … COMMIT` every `WRITE_BATCH_MS` (default
  100 ms). Every response waits for the commit.
- **Two read connections** (`READ_CONNS`), prepared statements, WAL,
  `synchronous=NORMAL`, 16 MiB page cache, 256 MiB mmap, `_mutex=no`.
- `Content-Length` is set; responses are not chunked.

## Local measurements

Docker load test with the server pinned to one CPU and `1.5 GiB` of memory,
four LG clients (20 s ramp, 60 s hold), fresh seed database, with read
coalescing disabled:

| Users | Client results | Server |
|---:|---|---|
| 35,000 | p95 71.6 ms, p99 120.1 ms, p50 0.4 ms, 0 errors; 4 clients pass | 61.5% CPU, 1.172 GiB peak |
| 70,000 | p95 ~10 s, ~40% errors; clients saw EOF/timeouts | container gone before the 45 s sample |

35k connections already use 1.17 GiB, so 70k does not fit this 1.5 GiB test
container and is not expected to fit the 2 GiB benchmark VM either. The
`READ_COALESCE=0` path is correct and passes the full test suite, but the
per-connection cost of stock `net/http` is the limit at these connection
counts.

`test/run.sh` passes **42/42** with `READ_COALESCE=0` (the default) and with
`READ_COALESCE=1`.

## Prior art

- SQLite-rendered JSON and prepared statements are common; e.g.
  `go-nethttp-fi5HiE` uses the same `json_object`/`json_group_array` approach.
- Group commit is common across the leaderboard.
- In-flight feed coalescing was independently implemented in the top-ranked
  `c-iouring-meros` submission, but left **off by default** because its author
  judged rule 5 (“no response/query-result caches”) ambiguous for one query
  serving many requests. This build also leaves it off by default; it is
  available with `READ_COALESCE=1`.
- The custom branch’s prebuilt packet and single-write response are also not
  novel: top io_uring implementations write headers in front of the body in one
  send.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` for the cgo driver.
- `build.sh` — module download/verify and a stripped build to `bin/server`.
- `start.sh` — foreground server.
