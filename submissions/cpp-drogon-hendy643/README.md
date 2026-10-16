# C++ + Drogon

| | |
|---|---|
| Language | C++26 (`-std=c++2c`), compiled with clang 20 (Ubuntu 24.04's `clang-20`) at `-O3 -march=native -flto`, linked with lld, release binary stripped |
| Framework | Drogon 1.9.13 (Trantor event loop) |
| Server | Drogon's own HTTP server, one event-loop thread per available CPU |
| SQLite driver | the C API, linked to Ubuntu's `libsqlite3` (3.45.1) |
| JSON | Glaze 8.0.0 |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

Drogon and Glaze are pinned by git tag in `CMakeLists.txt`. Everything else (OpenSSL, SQLite, zlib, jsoncpp, uuid, GoogleTest) is an Ubuntu package.

## Running it

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Trying it on a droplet-shaped box with Docker

`docker-compose.yml` has two profiles. Both pin the server to one CPU thread (`CPUSET`, default `0`) and limit RAM
to 2 GB with no swap and `nofile` to 65535, which are the droplet's limits. Each needs a directory holding a copy of
the seed database:

```bash
cp seed/feed.db /tmp/feed.db
```

**`direct`**: the server alone, with the full 2 GB, on the host network (Linux only). Host networking skips Docker's
port forwarding, which is the closest match to the droplet's direct setup. The container is not root and cannot bind
port 80 on the host, so it listens on `HOST_PORT` (default 8080) instead.

```bash
DB_DIR=/tmp docker compose --profile direct up --build
python3 -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:8080/health').read())"
```

**`nginx`**: the server on `127.0.0.1:3000` behind Nginx running the benchmark's `bench/nginx.conf` (one worker, as on a
one-vCPU box), on the same CPU. The 2 GB is split 1792 MB for the server and 256 MB for Nginx. The two containers
share one network namespace so Nginx's `127.0.0.1:3000` is the server, and port 80 is published on `HOST_PORT`
(default 8080) through Docker's port forwarding.

```bash
DB_DIR=/tmp docker compose --profile nginx up --build
```

Run only one profile at a time: they use the same CPU and port. Stop with `docker compose --profile <name> down`.
Then point `bench/load.js` at it, for example `k6 run -e BASE_URL=http://127.0.0.1:8080 -e VUS=2500 bench/load.js`.

## Optimizations, and why

- **One event-loop thread per available CPU, SQLite called inline on it.** The box has one vCPU, so a
  thread pool or extra processes would only add context switches. The thread count comes from the CPU
  affinity mask, so it is 1 on the droplet. Each thread has its own SQLite connection and its own
  prepared statements, so connections never contend for locks.
- **Direct instead of Nginx.** On one core, Nginx's CPU and its gzip of every `/feed` response would come
  straight out of the server's budget. The server listens on `0.0.0.0:80` and keeps idle connections for 75 s
  (Drogon's default is 60 s), which is also longer than the 65 s Nginx may hold one in its upstream pool, so it
  works behind `bench/nginx.conf` too. It never compresses.
- **File-descriptor limit raised in-process.** The server lifts its open-file limit to the hard limit and
  refuses new connections before it would run out, because Trantor aborts the process when `accept()` fails
  with `EMFILE`.
- **Hand-written routing.** One request advice matches the path and method itself, so Drogon's router,
  controllers and views are never involved.
- **JWT verified by hand** on every request: one OpenSSL HMAC-SHA256, a constant-time signature compare,
  HS256 only, `exp` checked. Glaze parses the header and claims. Nothing is cached (rule 5).
- **Glaze output into a reused buffer.** Each post is written straight from the SQLite row (string views,
  no copies) and appended to the response, so `/feed` makes no per-row allocations. Key order comes from
  the struct field order.
- **SQL**: the reference queries. Each like is a single statement,
  `INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`; a post-existence check runs
  only when it inserted nothing, to tell "already liked" from "no such post".
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 64 MiB page cache,
  in-memory temp store. Autocommit mode commits each write before its response is sent.

## Local results

Not a droplet run. Measured on an AMD Ryzen 9 5900X (12 cores, 24 threads, 32 GB RAM) with Docker confining the
server (and Nginx, for the second column) to **one CPU thread** and **2 GB of RAM with no swap**
(`docker-compose.yml`). It approximates the droplet's limits, not its CPU: a 5900X core is faster than a droplet
vCPU, so expect worse numbers on the real box.

| | Direct | Behind `bench/nginx.conf` |
|---|---|---|
| Path | k6 -> server on `0.0.0.0:8080` (Docker host network) | k6 -> Docker port forward -> Nginx (1 worker) -> server on `127.0.0.1:3000` |
| Memory limit | 2 GB for the server | 1792 MB server + 256 MB Nginx |
| `test/test.sh` | 42 of 42 | 42 of 42 |
| Requests | 33,861 (about 212 per second) | 33,886 (about 212 per second) |
| Failed | 0 (0.00%) | 0 (0.00%) |
| avg / median | 106 us / 100 us | 204 us / 187 us |
| p95 / p99 | 159 us / 220 us | 302 us / 420 us |
| max | 7.6 ms | 9.1 ms |

Both runs used `bench/load.js` with 2,500 users, a 30 s ramp and a 90 s hold (the scored run is a 5-minute hold),
with k6 on the same machine pinned to other cores, not on a separate one. The two columns are not like for like:
the Nginx profile has to go through Docker's port forwarding, which the direct profile's host networking avoids, so part
of the gap is Docker, not Nginx. A direct run through the same port forward measured p95 239 us / p99 301 us.

2,500 users is where the scoring procedure starts, and it is far below the limit: each user pauses 3 to 15 s
between requests, so the server sat at about 0% CPU. These numbers show that it works under the limits and that
Nginx costs a little latency, not how many users it can hold. The limit search (5,000 users upward, then a
5-minute hold) has not been run.

## License

MIT, under the repo's [license](../../LICENSE).
