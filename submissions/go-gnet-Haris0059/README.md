# Go + gnet

| | |
|---|---|
| Language | Go 1.27.1 |
| Server | gnet v2.10.0 (event loop) with a small hand-written HTTP/1.1 parser |
| SQLite | 3.53.4 amalgamation, compiled in via cgo; `build.sh` downloads it from sqlite.org and checks its sha256 |
| SQLite binding | a small cgo shim (`db.go`), no `database/sql` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

Go dependencies are pinned in `go.mod` / `go.sum`.

## Optimizations, and why

Measured on a $12 droplet (1 vCPU, 2 GB) with k6 on two other machines, and locally with the server pinned to one core.

- **One event loop (gnet) instead of a goroutine per connection.** On one vCPU, 40k idle keep-alive connections
  as goroutines cost memory and scheduler work. With one epoll loop, RSS at 40k connections is **~97 MB instead of
  ~930 MB** (fasthttp), and handlers plus SQLite run inline in the loop. The parser handles keep-alive, pipelining,
  `Content-Length` bodies and `Connection: close`. Chunked request bodies get a 400.
- **cgo shim instead of `database/sql`.** Every query is one cgo call that binds, steps, copies the result into a Go
  buffer and resets. There's one connection (opened `NOMUTEX`, serialized by a Go mutex), and statements are prepared
  once (`SQLITE_PREPARE_PERSISTENT`). Feed went 19.4 → 17.5 µs and post 3.5 → 2.1 µs, with 1 allocation instead of 11–12.
- **SQLite builds the response JSON** (`json_object` + `group_concat`). `/feed` and `/posts/:id` are one row each,
  ~40% cheaper than scanning 20 rows and encoding in Go. Only `POST /posts` uses the small Go encoder.
- **Like** is one statement (`INSERT ... SELECT ... WHERE EXISTS(post) ON CONFLICT DO NOTHING`). A follow-up
  existence check only runs when nothing was inserted.
- **Pragmas**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, 64 MiB page cache, `temp_store=MEMORY`. Writes are
  autocommitted (committed) before the response is written.
- **JWT** is verified by hand on every request: HS256 only, constant-time compare, `exp`/`nbf`. Nothing is cached.
- **GC**: `GOGC=400` with a 1.2 GiB soft memory limit.

Together, server CPU per 1k requests dropped from 49–50 ms (fasthttp + database/sql) to 40 ms locally. On the droplet at
40k users the server uses ~50–60% of the core (was ~70–86%).

**My own load test** used a $12 droplet in AMS3, with `bench/load.js` (60 s ramp, 1-min hold) run from three other
machines. **62,500 users passed** (p95 ~140 ms, p99 ~555 ms, 0 errors, 79% CPU, 126 MB RSS). 65,000 failed on latency
(p99 ~2 s at 85% CPU) with 0 errors and every connection held.

**Tried and rejected**: fasthttp with `ReduceMemoryUsage=false` (more CPU and memory), and multiple reader connections
(no gain on one core).

## License

MIT, under the repo's [license](../../LICENSE).
