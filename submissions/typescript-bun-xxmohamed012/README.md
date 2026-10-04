# TypeScript + Bun

| | |
|---|---|
| Language | TypeScript on Bun 1.4.2 (checksum verified in `install.sh`) |
| HTTP | Hand-written HTTP/1.1 server on `Bun.listen` |
| SQLite driver | `bun:sqlite` (bundled SQLite 3.53.2) |
| Build | `bun build --compile` to `bin/server` |
| Dependencies | None. `bun:ffi` is used for one libc call |
| Nginx or direct | **Direct** |

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
bun check.ts 80   # optional: split, pipelined and backpressured requests
```

## Design

- One event loop and one SQLite connection; the box has one vCPU.
- Requests are parsed from the socket buffer, with no `Request`/`Response` objects. Each response is one write
  from a preallocated buffer, so a keep-alive request costs one `recvfrom` and one `sendto`. On one core this
  was 6–9 µs per request against 9–12 µs for `Bun.serve`.
- Above 64,000 open connections, the least recently used ones are closed until 63,000 remain, as Nginx does
  when it runs out of connections. This lets the server handle more users than the 65,535 file descriptor
  limit. Connections are closed with RST, which avoids TIME_WAIT and keeps conntrack entries short-lived.
- The listen backlog is raised from Bun's fixed 512 to 4096 (capped by `somaxconn`) with a second `listen()`
  call, so bursts of reconnects don't cause dropped SYNs.
- SQLite renders the JSON with `json_object`; the feed is one query returning one row.
- `journal_mode=WAL`, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, 64 MiB page cache. Writes are
  autocommitted before the response is sent.
- JWTs are verified on every request with Bun's native HMAC (about 0.8 µs).
- The 500-character limit counts characters, as SQLite does, not UTF-16 units.

## Results

Local runs: Ubuntu 24.04 container, 1 CPU (Intel E-core), 2 GB, `nofile` 65535, 60 s ramp and 5 minute hold,
with a load generator that follows `bench/load.js`.

| Users | Errors | p95 | p99 | CPU | Peak memory |
|---|---|---|---|---|---|
| 60,000 | 0 | 0.3 ms | 1.6 ms | 18% | 331 MB |
| 100,000 | 0.014% | 7.4 ms | 18.9 ms | 39% | 384 MB |
| 200,000 | 0.004% | 11.1 ms | 14.7 ms | 75% | 463 MB |
| 250,000 | 0% | 455 ms | 822 ms | 100% | 613 MB |

Expect lower numbers on the droplet, which has a slower vCPU and handles all network processing itself.

## License

MIT, under the repository [license](../../LICENSE).
