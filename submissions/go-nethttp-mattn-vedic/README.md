# Go epoll + SQLite

| | |
|---|---|
| Language | Go 1.27.2 (official tarball, sha256-checked by `install.sh`) |
| HTTP server | Linux epoll with raw syscalls and direct endpoint dispatch |
| SQLite driver | Local `go-sqlite3` v1.14.24 fork, linked to Ubuntu's `libsqlite3` 3.45.1 (`3.45.1-1ubuntu2.8`) |
| Nginx or direct | Direct: listens on `HOST:PORT` |

## Pins

- **Go**: `install.sh` downloads `go1.27.2.linux-amd64.tar.gz` and checks its sha256. `build.sh` refuses any other version (`GOTOOLCHAIN=local`).
- **SQLite**: `install.sh` installs `libsqlite3-dev=3.45.1-1ubuntu2.8`, the exact Ubuntu noble-updates package. `build.sh` checks the header and runtime versions and refuses to build otherwise.
- **Go modules**: none from the network. The only Go dependency is the `go-sqlite3` fork in `./sqlite3`, which is in-tree. The build sets `GOPROXY=off` and `-mod=readonly`.

Run `sudo bash install.sh` once, then `bash build.sh`. Set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT`, then `bash start.sh`.

## Design

- A single Go process uses a Linux epoll event loop for HTTP/1.1 keep-alive connections.
- **Inline handlers.** With `HTTP_WORKERS=1` (the default), handlers run on the loop goroutine. A request does not hand off to another goroutine, so there is no job channel, no completion pipe, and no per-request epoll re-arming. With `HTTP_WORKERS>1`, the bounded worker pool is used instead.
- Each connection has at most one request in flight, which preserves response order. Later pipelined requests stay buffered until the response is written.
- Response headers and JSON bodies are sent with `sendmsg` as separate buffers. If a socket accepts only part of a response, the unsent bytes are retained until writable.
- Every handler queries SQLite for its result. No application response or table data is cached.
- HS256 JWTs are verified on every authenticated request, using a constant-time HMAC comparison. Expiration and payload types are validated.
- SQLite is accessed through the driver's direct connection and statement interfaces, without `database/sql`. A small fork addition, `SQLiteStmt.QueryText`, resets and binds the prepared statement, steps it, and copies its JSON text into a reusable buffer in one cgo call, leaving the statement reset.
- Writes use autocommit mode with WAL and `synchronous=NORMAL`. A successful write call returns after SQLite commits.
- Post reads use a single query with a correlated count, and the feed uses `posts_created_at_id_idx`. Writes use `RETURNING` and `INSERT ... ON CONFLICT`. Constant SQL statements are prepared once per connection.
- `HTTP_WORKERS` sets the worker count when it is above 1. `DB_READERS` defaults to the worker count, `DB_WRITERS` defaults to 1. The read-only pool runs in `mode=ro` and cannot write.

## Measured

All runs used the challenge's request loop (`bench/load-multi.js`): 50,000 VUs, 30-second ramp, 5-minute hold, four loopback addresses. The server ran as a systemd service limited to 1 CPU, 2 GiB, and no swap. k6 ran on the rest of the same 16-vCPU host, so these are not the official separate-machine numbers. Each configuration ran twice.

| Build | µs CPU/request | Server CPU during hold | p99 (ms) | Failed requests |
|---|---|---|---|---|
| Previous commit (worker pool, system SQLite) | 73.5 (73.1–73.9) | 37.9% | 4.01 | 0 |
| **This build** (inline handlers, system SQLite) | **58.3** (57.9–58.7) | **30.1%** | 3.77 | 0 |

Both runs of this build and of the previous commit completed the full hold with zero failed requests and zero check failures. At 50,000 VUs the server is about 30% busy, so latency is not the constraint and CPU per request is the number that matters.

Two alternatives were measured and not shipped:

- **Group commit** (batch each epoll pass's writes into one transaction) and **manual WAL checkpointing**: no measurable CPU gain at this write rate (about 60 µs per request either way), so they were removed.
- **A pinned SQLite 3.54.0 amalgamation** compiled with tuned flags: about 79 µs per request, consistently worse than Ubuntu's 3.45.1 at the server level. In isolation, 3.54 is faster for these queries, so the cause is in the server's use of SQLite and is not resolved.

## CPU and runtime profiling

Profiling is off unless `PPROF_ADDR` is set. It must resolve to a loopback address; the profiler will refuse to listen on a public interface. For example, start the app with `PPROF_ADDR=127.0.0.1:6060` while running a load test, then capture a 30-second CPU profile:

```bash
curl -o cpu.pprof 'http://127.0.0.1:6060/debug/pprof/profile?seconds=30'
go tool pprof -http=127.0.0.1:8081 cpu.pprof
```

The same listener exposes heap, goroutine, allocation, block, mutex, and runtime trace profiles under `/debug/pprof/`. Profiling adds overhead, so use it to locate hot paths and run performance comparisons with profiling disabled.

## License

MIT, under the repository's [license](../../LICENSE).
