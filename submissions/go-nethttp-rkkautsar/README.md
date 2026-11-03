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
  jobs in one `BEGIN IMMEDIATE … COMMIT`; it waits for `WRITE_BATCH_MS` only
  when at least four jobs are already queued, otherwise it commits
  immediately. Creates use one multi-row `json_each` insert; likes use one
  existence query plus one multi-row `INSERT … RETURNING`. Every response
  waits for the commit.
- **Four read connections** (`READ_CONNS`), prepared statements, WAL,
  `synchronous=NORMAL`, 16 MiB page cache, mmap disabled by default
  (`SQLITE_MMAP` bytes re-enables it), `_mutex=no`, and WAL autocheckpoint at
  10,000 pages. Connections use a 65 s idle timeout
  (`IDLE_TIMEOUT_MS`). It must exceed the load's longest think gap (15 s); the
  earlier 1 s timeout closed every keep-alive socket between requests, and
  clients do not retry a POST that lands on a socket the server just closed.
  A soft/hard resident cap (`SOFT_CONNS=44000`/`MAX_CONNS=46000`) bounds
  memory: above soft, read responses send `Connection: close` so the client
  reconnects after the response; writes are exempt so a chained create is not
  cut off. Above hard, the oldest idle socket is closed as a backstop.
- **Memory headroom.** The process sets a 1200 MiB Go soft memory limit when
  `GOMEMLIMIT` is unset, disables the 256 MiB SQLite mmap by default, and keeps
  the 16 MiB page cache. The resident cap sits below the official 45k stable
  point so excess clients recycle through `Connection: close` instead of
  pushing RSS over the 2 GiB box.
- `Content-Length` is set; responses are not chunked.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | SQLite database |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `IDLE_TIMEOUT_MS` | `65000` | Keep-alive idle timeout |
| `GOMEMLIMIT` | `1200MiB` | Go soft heap limit when unset in the environment |
| `MAX_CONNS` | `46000` | Hard resident-connection cap (LRU idle eviction) |
| `SOFT_CONNS` | `44000` | Above this, responses send `Connection: close` |
| `SQLITE_CACHE` | `16384` | SQLite page-cache size in KiB |
| `SQLITE_MMAP` | `0` | SQLite mmap size in bytes; 0 disables mmap |
| `WRITE_BATCH_MS` | `10` | Write group-commit window |
| `READ_CONNS` | `4` | SQLite read connections |
| `READ_COALESCE` | `1` | In-flight feed/post read sharing |
| `READ_BATCH_US` | `500` | Read coalescing window |
| `POST_BATCH` | `1` | Batched post-ID reads (`IN`) |

## Local measurements

Docker load test on one pinned CPU, 1.9 GiB memory, 1.7 GiB Go memory limit,
four LG clients (60 s ramp, 60 s hold), fresh seed database, tuned defaults.
The 47k row uses the new defaults, `NOFILE=65535`, `GOMEMLIMIT=1200MiB`,
`SQLITE_MMAP=0`, `SQLITE_CACHE=16384`, and a 120 s hold; the 70k/100k rows used
the earlier `SOFT_CONNS=48000`, `MAX_CONNS=50000`, 256 MiB mmap,
`IDLE_TIMEOUT_MS=65000`, and `POST_BATCH=1`. The defaults are now
`SOFT_CONNS=44000`, `MAX_CONNS=46000`, mmap disabled, and a 1200 MiB Go limit.

| Users | Client results | Server |
|---:|---|---|
| 47,000 (new defaults) | p95 13.7–14.2 ms, p99 121–127 ms, 0 errors, ~44k resident | 625 MiB at 30 s ramp, no OOM |
| 70,000 | p95 345–347 ms, p99 616–620 ms, 0 errors | 940 MiB at 30 s, ~48k resident, no OOM |
| 100,000 | p95 3.2 s, p99 5.5–6.2 s, 0.4–0.6 % net errors | OOM at 1.855 GiB |

The 70k run held ~48k resident connections; the excess cycled through
`Connection: close` after read responses (writes are exempt, so a chained
create is not cut off). 100k is past the one-vCPU/2 GiB ceiling: the ramp and
reconnect churn outrun the cap and the server OOMs.

The official service has `LimitNOFILE=65535`; the 46k hard cap stays below it,
so the fd limit is not the binding constraint. The official 5-minute rebench
held 45,000 users at 1,516 MB; 47,000 hit 1.56 GB and failed with 21.7%
`connection refused`. With the 65 s idle timeout, a net/http connection costs
roughly 29 KB including Go runtime and buffers, so the 44k soft cap leaves
headroom on the 2 GiB box instead of trying to hold 47k+ resident.

`test/run.sh` passes **42/42** with the default configuration.

## Prior art

- SQLite-rendered JSON and prepared statements are common; e.g.
  `go-nethttp-fi5HiE` uses the same `json_object`/`json_group_array` approach.
- Group commit is common across the leaderboard.
- In-flight feed coalescing was independently implemented in the top-ranked
  `c-iouring-meros` submission, but left **off by default** because its author
  judged rule 5 (“no response/query-result caches”) ambiguous for one query
  serving many requests. This build enables it by default; `READ_COALESCE=0`
  disables it. `POST_BATCH=1` (default) does the same for concurrent post
  reads; `POST_BATCH=0` disables it.
- The custom branch’s prebuilt packet and single-write response are also not
  novel: top io_uring implementations write headers in front of the body in one
  send.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` for the cgo driver.
- `build.sh` — module download/verify and a stripped build to `bin/server`.
- `start.sh` — foreground server.
