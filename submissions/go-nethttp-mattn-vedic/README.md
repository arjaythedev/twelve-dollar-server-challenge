# Go epoll + SQLite

| | |
|---|---|
| Language | Go (Ubuntu 24.04 `golang-go`) |
| HTTP server | Linux epoll with raw syscalls and direct endpoint dispatch |
| SQLite driver | Local `go-sqlite3` v1.14.24 fork linked to Ubuntu's system SQLite |
| Nginx or direct | Direct: listens on `HOST:PORT` |

Go dependencies are pinned in `go.mod`/`go.sum`. The build uses CGO and installs GCC and `libsqlite3-dev` on the clean Ubuntu host.

## Design

- A single Go process uses a Linux epoll event loop for HTTP/1.1 keep-alive connections. A bounded worker pool runs handlers and database calls, keeping SQLite cgo work off the socket loop. Each connection has at most one request in flight, preserving HTTP response order; later pipelined requests remain buffered until that response is written. The loop reuses its read buffer and retains per-connection bytes only for incomplete or in-flight requests. Idle connections remain open for 120 seconds.
- Every handler queries SQLite for its result. No application response or table data is cached.
- HS256 JWTs are verified on every authenticated request using a constant-time HMAC comparison. Expiration and payload types are validated.
- SQLite is accessed through the driver's direct connection and statement interfaces, without `database/sql`. A small local `go-sqlite3` fork adds `SQLiteStmt.QueryText`, which resets and binds the prepared statement, steps it, and copies its JSON text into a reusable Go buffer in one cgo call. SQLite's C amalgamation is not vendored; the build links Ubuntu's system SQLite. Writes use autocommit mode with WAL and `synchronous=NORMAL`; a successful write call returns after SQLite commits.
- Post reads use a single query with a correlated count. Each feed request runs one prepared SQLite query that returns the newest posts and like counts as JSON. Writes use `RETURNING` and `INSERT ... ON CONFLICT`.
- Constant SQL statements are prepared once per connection and reused. SQLite's query plan uses `posts_created_at_id_idx` for feed ordering and primary-key lookups for posts/users. The feed fast path uses the covering `likes_post_id_idx` over only the selected ID range; post-by-ID reads keep the indexed correlated count.
- Direct serving avoids a separate Nginx process on the single vCPU host.

Set `HTTP_WORKERS` to a positive integer to set the handler worker count (default 1 for the challenge's single-vCPU target). `DB_READERS` defaults to the worker count; `DB_WRITERS` defaults to 1. SQLite serializes writes to a database file, so additional writer connections do not make writes execute in parallel. The read-only pool runs in `mode=ro` and cannot issue writes. The worker count is configurable because the best setting depends on the CPU quota and workload.

Run `sudo bash install.sh`, `bash build.sh`, then set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT` before `bash start.sh`.

## CPU and runtime profiling

Profiling is off unless `PPROF_ADDR` is set. It must resolve to a loopback address; the profiler will refuse
to listen on a public interface. For example, start the app with `PPROF_ADDR=127.0.0.1:6060` while running
a load test, then capture a 30-second CPU profile and open the pprof flame graph:

```bash
curl -o cpu.pprof 'http://127.0.0.1:6060/debug/pprof/profile?seconds=30'
go tool pprof -http=127.0.0.1:8081 cpu.pprof
```

The profile attributes sampled CPU time to functions and call stacks. The same listener exposes heap,
goroutine, allocation, block, mutex, and runtime trace profiles under `/debug/pprof/`. Profiling adds
overhead, so use it to locate hot paths and run performance comparisons with profiling disabled.

Requests can queue in the bounded worker channel while the epoll loop continues servicing sockets; the
per-connection in-flight limit preserves response order.

## Benchmark snapshot

On the local 1-vCPU / 2-GiB server cgroup, a 90,000-VU run with one HTTP worker completed with zero failed
requests: 7,536 requests/s, p95 424 ms, and p99 1.19 s. This met the p95 target but missed the p99 target.
The server's measured cgroup memory peak was about 457 MiB. The load generator ran uncapped on the host
using four loopback aliases. CPU, heap, allocation, and goroutine profiling were enabled, so treat this as
a profiling run rather than a clean capacity claim.

In the earlier one-worker run, throughput was 7,797 requests/s, p95 472 ms, and p99 1.57 s. This run's
p95 and p99 were lower, while throughput was about 3% lower. Each result is a single run, so this is a
directional comparison rather than a conclusive performance gain. A 1/2/4-worker comparison also favored
one worker for this single-vCPU cgroup; four workers caused substantially more CPU throttling. Tune for
the actual CPU quota and workload.

## License

MIT, under the repository's [license](../../LICENSE).
