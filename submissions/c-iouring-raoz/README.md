# C + io_uring

| | |
|---|---|
| Language | C (GNU C11), gcc 13, `-O3 -march=native`, LTO, profile-guided optimization |
| Framework | None. The HTTP/1.1 server uses io_uring directly. |
| Libraries | liburing 2.15 and the SQLite 3.53.4 amalgamation, both pinned and SHA-256 checked |
| **Nginx or direct** | **Direct**: the server listens on `0.0.0.0:80` |
| Kernel | Linux 6.1 or later |

## Run

```bash
sudo bash install.sh   # build-essential, curl
bash build.sh          # downloads and checks the two tarballs, builds bin/server (1-2 minutes)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The server reads only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.

## Results

The server ran as a systemd service with `LimitNOFILE=65535` on a box that was not tuned. The rows
that name k6 used k6 with `bench/load.js`. The other rows used our load generator,
[`tests/loadgen.c`](tests/loadgen.c). It follows the same user loop as `bench/load.js`. A user of the
load generator costs about 1 KB, and a k6 user costs about 280 KB. Thus one client machine can run
hundreds of thousands of users.

**DigitalOcean `s-1vcpu-2gb`** (Ubuntu 24.04, kernel 6.8), with the load from 6 droplets in the same VPC:

| Users | Requests/s | Errors | p95 | p99 | Result |
|---|---|---|---|---|---|
| 100,000 | 10,600 | 0 | 15 ms | 28 ms | pass |
| 125,000 | 13,200 | 0 | 166 ms | 5.7 s | fail: the vCPU is full |

The limit is about 110,000 users. The CPU sets this limit, and the kernel network stack uses more
than half of it. On an 8-vCPU droplet, k6 itself did not have sufficient CPU to measure 40,000 users
correctly.

**AWS `c7a.medium`** (1 dedicated core, 1.9 GB):

| Test | Requests/s | Errors | p95 | p99 |
|---|---|---|---|---|
| k6 with `bench/load.js`, 60,000 users | 5,300 | 0 | 8 ms | 31 ms |
| 300,000 users | 31,000 | 0.00% | 43 ms | 59 ms |
| No think time, 256 connections | 54,500 | 0 | 6 ms | 31 ms |

Each connection uses about 4.5 KB of kernel memory. Thus 2 GB of RAM holds about 300,000 connections.

## Design

- **One event loop does the work.** The loop uses multishot accept, multishot recv and batched sends.
  When the server is busy, the kernel wakes the loop after 128 completions or 4 ms.
- **SQLite runs on the loop thread.** Each request is a few B-tree lookups in the SQLite page cache.
  The database uses WAL with `synchronous=NORMAL` and `locking_mode=EXCLUSIVE`.
- **Group commit.** The writes of one batch share one transaction. The server sends no response of the
  batch before the commit. If the commit fails, each response of that transaction becomes a 500.
- **The feed uses three range scans.** The 20 newest posts have almost always adjacent ids. One scan
  reads the ids, one counts the likes, and one reads the rows. If the ids are far apart, the server
  uses the reference query.
- **More than 65,535 connections.** Each socket goes into an io_uring file table, not the process file
  table. Each of the 8 worker threads has a table of 65,535 slots. When a table is almost full, the
  next worker accepts the new connections.
- **Memory limits.** The server reads `/proc/meminfo` four times each second. Below 192 MB of usable
  memory, it stops accepting connections. Below 96 MB, it closes idle connections. If the kernel kills
  the server, `start.sh` starts it again.
- **Clients that do not behave.** If a connection has more than 64 KiB of unsent responses, the server
  stops reading from it. If a partial request or a response makes no progress for 30 s, the
  server closes the connection.

The request path is written by hand: the HTTP parser, a strict JSON parser, HMAC-SHA256 and the JSON
writer. The server checks the JWT signature, `exp` and `nbf` on every request.

## Rules

- No data is cached between requests. Each response comes from SQLite queries for that request.
- Each write is committed before the server sends its response.
- The schema is not changed. `PRAGMA foreign_keys=ON` makes the `REFERENCES` clauses active.

## Tests

The contest test `test/test.sh` passes. The `tests/` directory has more tests in C and the load
generator. `build.sh` does not build them. To run the tests, use `make -C tests check` after
`build.sh`. See [tests/README.md](tests/README.md).

## License

MIT, under the [license](../../LICENSE) of the repository.
