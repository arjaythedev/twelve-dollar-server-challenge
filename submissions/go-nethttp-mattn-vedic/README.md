# Go epoll + SQLite

| | |
|---|---|
| Language | Go (Ubuntu 24.04 `golang-go`) |
| HTTP server | Linux epoll with raw syscalls and direct endpoint dispatch |
| SQLite driver | Local `go-sqlite3` v1.14.24 fork with the bundled SQLite amalgamation |
| Nginx or direct | Direct: listens on `HOST:PORT` |

Dependencies are pinned in `go.mod`/`go.sum`. The build uses CGO and installs GCC on the clean Ubuntu host.

## Design

- A single Go process uses a Linux epoll event loop for HTTP/1.1 keep-alive connections. A bounded worker pool runs handlers and database calls, keeping SQLite cgo work off the socket loop. Each connection has at most one request in flight, preserving HTTP response order; later pipelined requests remain buffered until that response is written. The loop reuses its read buffer and retains per-connection bytes only for incomplete or in-flight requests. Idle connections remain open for 120 seconds.
- Every handler queries SQLite for its result. No application response or table data is cached.
- HS256 JWTs are verified on every authenticated request using a constant-time HMAC comparison. Expiration and payload types are validated.
- SQLite is accessed through `go-sqlite3`'s direct connection and statement interfaces, without `database/sql`. Its small local fork adds `SQLiteRows.NextText`, which copies SQLite's JSON text into a per-reader reusable byte buffer without allocating a Go string per result. Writes use autocommit mode with WAL and `synchronous=NORMAL`; a successful write call returns after SQLite commits.
- Post reads use a single query with a correlated count. Each feed request runs one prepared SQLite query that returns the newest posts and like counts as JSON. Writes use `RETURNING` and `INSERT ... ON CONFLICT`.
- Constant SQL statements are prepared once per connection and reused. SQLite's query plan uses `posts_created_at_id_idx` for feed ordering and primary-key lookups for posts/users. The feed fast path uses the covering `likes_post_id_idx` over only the selected ID range; post-by-ID reads keep the indexed correlated count.
- Direct serving avoids a separate Nginx process on the single vCPU host.

Set `HTTP_WORKERS` to a positive integer to set the handler worker count (default 4). `DB_READERS` defaults to the worker count; `DB_WRITERS` defaults to 1. SQLite serializes writes to a database file, so additional writer connections do not make writes execute in parallel. The read-only pool runs in `mode=ro` and cannot issue writes. The worker count is configurable because the best setting depends on the CPU quota and workload.

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

On the local 1-vCPU / 2-GiB server cgroup, a 90,000-VU run with one HTTP worker completed with no failed
requests: 7,797 requests/s, p95 472 ms, and p99 1.57 s. This met the p95 target but did not meet the p99
target. The load generator ran uncapped on the host, using four loopback aliases; it reached roughly 24 GiB
RSS and drove host swap during the run, so these figures describe this test setup rather than a clean,
production-grade capacity claim. The server's measured cgroup memory peak was about 443 MiB.

The same comparison measured one, two, and four workers. One worker had the best measured throughput and
tail latency on this single-vCPU cgroup; the four-worker run consumed more CPU and was heavily throttled.
Run-to-run p99 results were noisy under host memory pressure. These measurements are useful for tuning the
worker count, not a guarantee for different hardware or load generators.

## License

MIT, under the repository's [license](../../LICENSE).
