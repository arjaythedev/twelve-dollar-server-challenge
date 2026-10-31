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
- **In-flight read batching.** Concurrent requests for the same feed/post join
  one SQLite read. The leader waits `READ_BATCH_US` (default 500 µs) so other
  handlers can join; the group is deleted as soon as the read completes. A
  generation counter in the key is bumped after every commit, so a request that
  starts after a 201/200 cannot join a pre-write read. There is no cache that
  survives a completed group.
- **Write group commit.** One dedicated `*sql.Conn` runs queued create/like
  jobs in one `BEGIN IMMEDIATE … COMMIT` every `WRITE_BATCH_MS` (default
  100 ms). Every response waits for the commit.
- **Two read connections** (`READ_CONNS`), prepared statements, WAL,
  `synchronous=NORMAL`, 16 MiB page cache, 256 MiB mmap, `_mutex=no`.
- `Content-Length` is set; responses are not chunked.

## Local measurements

Server pinned to 1 CPU, 100 keep-alive clients, same seed DB:

| Path | This build | Custom branch | Current net/http peers |
|---|---:|---:|---:|
| `/feed` | 10.4–12.1k req/s, 81–87 µs | 19.2–19.9k, 47 µs | 5.9–6.9k |
| `/posts/:id` | 16.2–17.4k req/s, 55–58 µs | 26.7–26.9k, 33 µs | 10.4–10.9k |

`test/run.sh` passes **42/42**. The custom branch (`go-nethttp-rkkautsar` on its
own branch) keeps the custom parser/writer and direct cgo binding.

## Prior art

- SQLite-rendered JSON and prepared statements are common; e.g.
  `go-nethttp-fi5HiE` uses the same `json_object`/`json_group_array` approach.
- Group commit is common across the leaderboard.
- In-flight feed coalescing was independently implemented in the top-ranked
  `c-iouring-meros` submission, but left **off by default** because its author
  judged rule 5 (“no response/query-result caches”) ambiguous for one query
  serving many requests. This build keeps it enabled as request batching; no
  result outlives its group.
- The custom branch’s prebuilt packet and single-write response are also not
  novel: top io_uring implementations write headers in front of the body in one
  send.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` for the cgo driver.
- `build.sh` — module download/verify and a stripped build to `bin/server`.
- `start.sh` — foreground server.
