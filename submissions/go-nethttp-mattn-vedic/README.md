# Go net/http + SQLite

| | |
|---|---|
| Language | Go (Ubuntu 24.04 `golang-go`) |
| HTTP server | Go standard library `net/http` |
| SQLite driver | `github.com/mattn/go-sqlite3` v1.14.24, linked with system SQLite |
| Nginx or direct | Direct: listens on `HOST:PORT` |

Dependencies are pinned in `go.mod`/`go.sum`. The build uses CGO and installs GCC on the clean Ubuntu host.

## Design

- A single Go process uses separate SQLite reader and writer pools. Reader connections open the database read-only; writes use a WAL-enabled pool. The standard server uses goroutines and supports keep-alive; idle connections remain open for 75 seconds.
- Every handler queries SQLite for its result. No application response or table data is cached.
- HS256 JWTs are verified on every authenticated request using a constant-time HMAC comparison. Expiration and payload types are validated.
- `database/sql` executes writes in autocommit mode with SQLite WAL and `synchronous=NORMAL`; a successful write call returns after SQLite commits.
- Feed and post reads use a single query with a correlated count, and writes use `RETURNING` and `INSERT ... ON CONFLICT`.
- Constant SQL statements are prepared once per pool and reused. SQLite's query plan uses `posts_created_at_id_idx` for feed ordering, primary-key lookups for posts/users, and the covering `likes_post_id_idx` for each correlated like count. The correlated count is retained so the feed can stop after its 20 indexed posts instead of aggregating likes for the whole database.
- Direct serving avoids a separate Nginx process on the single vCPU host.

Set `DB_READERS` and `DB_WRITERS` to positive integers to cap the open and idle connections in each pool. Defaults are 8 readers and 1 writer. SQLite still serializes writes to a database file, so increasing `DB_WRITERS` allows more concurrent write attempts but does not make SQLite perform those writes in parallel; tune it with the benchmark. The read-only pool runs in `mode=ro` and cannot issue writes.

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

Every 10 seconds, the process logs reader and writer pool open/in-use/idle connection counts and the change
in `database/sql` wait count and cumulative wait duration over that interval. A nonzero wait delta means
requests queued for a pool connection; compare the reader and writer deltas before changing `DB_READERS`
or `DB_WRITERS`.

## License

MIT, under the repository's [license](../../LICENSE).
