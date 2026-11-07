# Golang + FastHTTP

| | |
|---|---|
| **Language** | Go 1.27.3  |
| **HTTP Engine** | [`fasthttp`](https://github.com/valyala/fasthttp) v1.77.0 |
| **SQLite Driver** | [fork](https://github.com/riyaz-ali/sqlite3) of [crawshaw's sqlite3 driver](https://github.com/crawshaw/sqlite) with `v3.53.0` |
| **JSON Serialization** | [`fastjson`](https://github.com/valyala/fastjson) |
| **Nginx or Direct** | Direct on `:80` |

All Go dependencies are pinned in `go.mod` / `go.sum`

## Optimizations

**One SQLite connection, exclusive locking.** The server holds a single connection behind a mutex. On 1 vCPU a pool
buys no parallelism, and one connection allows `locking_mode=EXCLUSIVE`: SQLite keeps the WAL index in heap memory and
stops taking file locks for every read transaction (several `fcntl()` calls per statement otherwise). WAL with
`synchronous=NORMAL`, so every 201 is committed before the response is sent. Note that, as a result, nothing else can
open the database file while the server is running.

**No per-request pool overhead.** `sqlitex.Pool.Get` creates a cancellable context, a tracer and an interrupt goroutine
per call. The shared connection is handed out with a plain mutex instead, and `conn.Prep` caches each statement on it
(statements cache the query plan, not the data).

**Keep-alive, shed past the fd limit.** Connections stay open between requests until 7/8 of the fd limit (~57k open
connections with `LimitNOFILE=65535`). Past that, responses carry `Connection: close`, so clients give their fd back
and reconnect on their next request instead of waiting in the listen queue. Idle connections are dropped after 30 s
(direct mode only; the load test's longest pause between requests is ~23 s, so this only reaps users who left).
fasthttp's `Serve` also returns (and the process exits) the first time `accept()` fails with `EMFILE`, so the listener
is wrapped to retry instead. `ReduceMemoryUsage` frees per-connection buffers while connections are idle (about 6 KB
per idle connection).

**GC.** `GOGC=400` with a 1.2 GB soft memory limit.

## License

MIT, under the repo's [license](../../LICENSE).
