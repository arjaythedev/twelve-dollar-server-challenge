# go-nethttp-fi5HiE

An ordinary Go web service: the standard library's `net/http` and `database/sql`, nothing custom.

I wanted to see how far a normal Go service gets when you write it the way you would for
production. So there is no custom event loop, no hand-rolled HTTP, JSON or JWT, and no application
logic in C. The only C in the build is SQLite itself, through the stock `mattn/go-sqlite3` driver.
The tuning is limited to in-process settings an operator would set anyway.

I tested it in a container shaped like the droplet (1 vCPU, 1.75 GB, no swap, on a 2011-era Xeon
core, since I did not have a real droplet) with a Go load generator replaying `bench/load.js`, using
a 60 s ramp and 5-minute holds. 65k users came in at p95 231 ms and p99 432 ms. 70k users passed
three runs out of three, at p95 330-350 ms and p99 555-586 ms. 75k fails, and it fails on accepting
connections (the listen queue overflows at about 62k open connections) rather than on serving them.

| | |
|---|---|
| Language | Go 1.27.1 |
| HTTP | `net/http` (stdlib), `ServeMux` method+path routing |
| SQLite | [`github.com/mattn/go-sqlite3`](https://github.com/mattn/go-sqlite3) v1.14.52 (cgo, compiles its bundled SQLite amalgamation from source) |
| JWT | [`github.com/golang-jwt/jwt/v5`](https://github.com/golang-jwt/jwt) v5.3.1, HS256 only, `exp` required |
| Deployment | **Direct** on `HOST:PORT` (`0.0.0.0:80`), no Nginx |

## What makes it fast

- **Direct, no Nginx**: Nginx mode times out new connections above ~55k users, while direct passed 70k three
  times over.
- **cgo SQLite** (`mattn/go-sqlite3`) over pure Go: ~30% less CPU per request.
- **SQLite renders `/feed`** with `json_object`/`json_group_array`: one text value instead of
  100 column reads, 28.6 to 18.9 µs and 184 to 27 allocations per request. The array keeps the LIMIT
  subquery's order, so there is no second ORDER BY (that alone was ~15% of the query).
- **One-statement like**: `INSERT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`.
- **Prepared statements** for every query, reused across requests.
- **Separate read and write pools**: `2 x GOMAXPROCS` (min 4) read-only connections and one writer,
  so WAL readers never wait and writes queue in Go instead of spinning on `SQLITE_BUSY`.
- **`_mutex=no`**: drops SQLite's per-connection lock (~20% of read CPU).
- **No request context on queries**: avoids a watcher goroutine per query (4-30% per endpoint).
- **`SQLITE_DEFAULT_MEMSTATUS=0`**: no global mutex on every SQLite allocation (feed -3%).
- **Streamed JSON** with `json.NewEncoder(w)`: no extra copy of each response.
- **`IdleTimeout` 10 s**: keeps most users connected between requests (5 s cost ~8% more CPU
  reconnecting, and 15 s or more hits the 65,535 fd limit).
- **`GOMEMLIMIT=1500MiB`**: GC paces itself to the 2 GB box, so ~55k open connections fit.
- **WAL + `synchronous=NORMAL`**: the durability the rules require (a 201 only after the commit),
  without an fsync per commit.
