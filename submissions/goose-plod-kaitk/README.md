# Goose / Plod / SQLite

The feed API written as an ordinary [Plod](https://github.com/kaitk/plod) application:
routes, a bearer check, and SQL with bound parameters, on the framework's own HTTP
server. There is no hand-written event loop, request parser, or B-tree code here, and
no C: `app.goose` is the whole application, about 250 lines.

| | |
|---|---|
| Language | [Goose](https://github.com/aardappel/goose) release `v2026.4`, which compiles to C; the official release, checked against its SHA-256 by `install.sh` |
| Framework | [Plod](https://github.com/kaitk/plod) `v0.5.12`, a REST framework on the Actix model, on [goose-http](https://github.com/kaitk/goose-http) `v0.5.19` (io_uring) |
| SQLite | [goose-sqlite](https://github.com/kaitk/goose-sqlite) `v0.0.4`, which builds SQLite 3.54.1's amalgamation, unmodified, into the program |
| JWT | Plod's HS256 verifier, over [goose-hash](https://github.com/kaitk/goose-hash) `v0.0.4` |
| C compiler | Ubuntu 24.04's gcc, at `goose build`'s defaults (`-O2`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ for goose-http's io_uring loop |

## How it works

- **The routes** are `plod::get` and `plod::post` calls in one `plod::routes` block.
  `POST /posts` and `POST /posts/:id/like` run inside `author`, a middleware block that
  checks the bearer token first and passes the user id and name to the route, so a
  request without a token gets 401 before its id is read, as SPEC.md asks.
- **Queries run in the route.** SQLite answers from its page cache, so a query is CPU
  work, and a separate database thread would only move it to another thread on the
  same core. goose-sqlite's `Conn` prepares each statement once and keeps it, and a
  query is its SQL and its values:
  `sqlite::query(db, ONE_POST, id) { r => post(out, r); }`.
- **Each write commits before its response.** `INSERT ... RETURNING` for a post, and
  `INSERT ... SELECT ... ON CONFLICT DO NOTHING` for a like, each its own transaction,
  in WAL with `synchronous=NORMAL` (rule 6). A like that changes nothing is a repeat,
  or a post that doesn't exist, which one more lookup tells apart.
- **One process.** Every k6 user keeps its connection open, and a process can hold
  only `LimitNOFILE` descriptors, 65,535 on the box. goose-http's `ring_direct` keeps
  connections as io_uring direct descriptors instead, in rings of 65,535 slots that
  the worker adds as they fill, so one process holds them all. Several processes
  sharing the database cost twice the CPU: each commit in one throws away the others'
  page caches.
- **Batched waits.** `ring_batch_us: 1000` lets a wait hold up to 1 ms for more
  completions, so the worker wakes once for many requests, not once each.
- **1,024 receive buffers.** Each connection with data in a pass takes one of the
  ring's 4 KB receive buffers. With goose-http's default of 64, the other
  connections' receives end with `ENOBUFS` and are queued again, pass after pass, so
  CPU climbs with the number of connections ready at once. `ring_buffers: 1024` costs
  4 MB.
- **The connection** is opened `NO_MUTEX`, since only its worker uses it: a feed
  request reads 100 columns, and each read would otherwise take a lock.
- **The single post's query** reads the author with a subquery, not a join. With the
  join, SQLite's constant propagation reads the bound id while planning, and
  re-prepares the statement for every request.
- **A checkpoint lane.** Copying the WAL back into the database waits on fsyncs, and
  on the worker that wait stalls every connection. A goose-http lane, a thread with
  its own connection, runs a PASSIVE checkpoint every 1,024 writes, about 4,000 WAL
  pages; PASSIVE never holds up the worker's writes. Writes keep arriving while it
  runs, so it never empties the WAL itself: the worker's own checkpoint, at 16,000
  pages (`wal_autocheckpoint`), copies only what is left and lets the WAL start over.
- **The `unix-excl` VFS.** Both connections open the database as a `file:` URI on
  SQLite's `unix-excl` VFS, as the one process holds the database alone: the file is
  locked once, and the two connections share the WAL index in memory, with no `-shm`
  file and no file locks per transaction.
- **Pragmas**: `cache_size` 16 MB on the worker, which leaves page reads at 0.03 a
  request. No `mmap`: with it, every page read looks the page up in the WAL index,
  which was a third of the instructions in a profile.
- **JSON strings** go through `http::json_string`, which finds what to escape with
  std's `find_any`, not a byte-at-a-time loop.

## Measured

The first version, without `ring_buffers` or the lane, held 115,000 users on the
board; it ran at half the CPU during the hold, and at 120,000 failed at 97% with
timeouts. Past the CPU it has, connections queue, many are ready in each pass, and
64 buffers leave the rest re-queuing their receives.

On x86-64: an AMD Ryzen 7 5800X3D, Ubuntu 26.04 in WSL2 on Linux
6.18.58.1-microsoft-standard-WSL2, Docker 29.8.2; the server in a container of 2 GB,
k6 1.3.0 in another, `bench/load.js` with its think times divided by 20, 60 s after a
15 s ramp, the three builds alternating, three rounds; kensac once at each load.
With the server held to 0.4 CPU so that k6 saturates it, at 10,000 users:

| Build | req/s | p95 | p99 | max | µs a request |
|---|---:|---:|---:|---:|---:|
| first version, 64 buffers | 9,897 to 9,931 | 529 to 551 ms | 623 to 878 ms | 853 to 2,493 ms | 35.1 to 35.2 |
| 1,024 buffers | 10,982 to 11,181 | 413 to 432 ms | 501 to 532 ms | 585 to 765 ms | 31.2 to 31.7 |
| 1,024 buffers and the lane (`start.sh`) | 11,274 to 11,405 | 331 to 348 ms | 366 to 475 ms | 452 to 629 ms | 30.9 to 31.4 |
| rust-iouring-kensac | 13,454 | 184 ms | 204 ms | 286 ms | 25.6 |

Below capacity, at 1 CPU and 4,000 users (6,500 requests a second):

| Build | req/s | p95 | p99 | max | µs a request |
|---|---:|---:|---:|---:|---:|
| first version, 64 buffers | 6,557 to 6,563 | 1.7 ms | 116 to 124 ms | 218 to 237 ms | 32.3 to 32.6 |
| 1,024 buffers | 6,544 to 6,563 | 1.7 ms | 115 to 123 ms | 237 to 254 ms | 32.3 to 32.6 |
| 1,024 buffers and the lane (`start.sh`) | 6,586 to 6,591 | 1.6 ms | 2.5 to 2.7 ms | 58 to 82 ms | 32.5 to 32.7 |
| rust-iouring-kensac | 6,588 | 1.4 ms | 1.8 ms | 28 ms | 27.1 |

The buffers give 12% more requests a second past capacity, at 11% less CPU a
request, and change nothing below it. The lane takes the checkpoint's wait off the
worker: below capacity, that wait was the whole p99.

In Docker on an M1 Pro (Linux arm64), the server held to 0.4 CPU so that k6
saturates it (20,000 users, think times divided by 10, 60 s after a 15 s ramp), two
alternating runs each, before the lane:

| Build | req/s | p95 | p99 | µs a request |
|---|---:|---:|---:|---:|
| 64 buffers | 13,084 to 13,110 | 459 to 634 ms | 693 to 1,075 ms | 24.9 to 25.3 |
| 1,024 buffers | 14,805 to 15,021 | 200 to 261 ms | 329 to 393 ms | 21.7 to 22.2 |

Below saturation the two are level: at 1 CPU and 8,000 requests a second, p99 11 to
33 ms against 10 to 26 ms, and 21.5 to 21.7 µs a request against 21.3 to 23.3,
three runs each. On the Mac the lane was level with the buffers alone at both
loads, and cut p99 in half only with fsyncs slowed to 20 ms.

The measurements below are of the first version, at 4,000 users.

In Docker on an M1 Pro (Linux arm64), the server in a container of 1 CPU and 2 GB, k6
in another, with `bench/load.js`'s think times divided by 20 so that 4,000 users make
6,300 requests a second. CPU from the container's cgroup:

| Server | µs a request | p99 |
|---|---:|---:|
| rust-iouring-kensac, the board's first | 15.6 to 16.2 | 2.4 to 2.6 ms |
| this entry (`start.sh`) | 21.4 to 22.0 | 4.3 to 9.5 ms |
| this entry before direct descriptors, 3 processes | 42.0 to 42.7 | 1.6 to 5.3 ms |

On x86-64, in Docker on WSL2 (Linux 6.18) on a Ryzen 7 5800X3D, the same setup, three
alternating runs each:

| Server | µs a request | user | kernel | p99 |
|---|---:|---:|---:|---:|
| rust-iouring-kensac | 27.2 to 27.5 | 4.6 to 4.7 | 22.5 to 22.9 | 1.9 to 2.3 ms |
| this entry (`start.sh`) | 32.5 to 33.3 | 10.1 to 10.3 | 22.3 to 23.2 | 52 to 118 ms |

Kernel time there is mostly Docker's bridge, the same for both. Most of the entry's user
time is SQLite running the SQL, about 7.6 µs a request. The p99 comes from writes:
without them it is 2 ms.

These are not the box's numbers. Scaled by the ratio to kensac, the entry estimates
between 225,000 and 260,000 users.

## Running it

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/tmp/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```
