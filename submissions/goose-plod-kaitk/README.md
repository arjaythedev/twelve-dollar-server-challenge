# Goose / Plod / SQLite

The feed API written as an ordinary [Plod](https://github.com/kaitk/plod) application:
routes, a bearer check, and SQL with bound parameters, on the framework's own HTTP
server. There is no hand-written event loop, request parser, or B-tree code here, and
no C: `app.goose` is the whole application, about 250 lines.

| | |
|---|---|
| Language | [Goose](https://github.com/aardappel/goose) release `v2026.4`, which compiles to C; the official release, checked against its SHA-256 by `install.sh` |
| Framework | [Plod](https://github.com/kaitk/plod) `v0.5.11`, a REST framework on the Actix model, on [goose-http](https://github.com/kaitk/goose-http) `v0.5.16` (io_uring) |
| SQLite | [goose-sqlite](https://github.com/kaitk/goose-sqlite), which builds SQLite 3.54.1's amalgamation, unmodified, into the program |
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
- **Three processes.** Every k6 user keeps its connection open, and one process can
  hold only `LimitNOFILE` sockets, 65,535 on the box. `start.sh` starts three
  processes, one worker each, on the same port through `SO_REUSEPORT`, which take up to
  192,000 connections between them, and stops them all when one exits. Each process
  more costs CPU, about 8 µs a request in our runs, so there are as few as the
  connections need. They share the database as SQLite's WAL lets processes share it.
  A writer holds the write lock for one statement; another waits for it by yielding
  the CPU, which on one core is what the holder needs, rather than sleeping a
  millisecond and stalling its process's other connections.
- **The connection** is opened `NO_MUTEX`, since only its worker uses it: a feed
  request reads 100 columns, and each read would otherwise take a lock.
- **The single post's query** reads the author with a subquery, not a join. With the
  join, SQLite's constant propagation reads the bound id while planning, and
  re-prepares the statement for every request.
- **Pragmas**: `cache_size` 16 MB a process, and `wal_autocheckpoint` 4,000 pages, so
  a checkpoint's `fsync` comes about once every 1,000 writes rather than every 250. No
  `mmap`: with it, every page read looks the page up in the WAL index, which was a
  third of the instructions in a profile.

## Measured

In Docker on an M1 Pro (Linux arm64), the server in a container of 1 CPU and 2 GB, k6
in another, with `bench/load.js`'s think times divided by 20 so that 4,000 users make
6,300 requests a second. CPU from the container's cgroup:

| Server | µs a request | p99 |
|---|---:|---:|
| rust-iouring-kensac, the board's first | 16.0 | 2.5 ms |
| this entry, 1 process | 28.6 | 5.8 ms |
| this entry, 3 processes (`start.sh`) | 44.4 | 4.7 ms |

These are not the box's numbers. Scaled by the ratio to kensac, three processes
estimate about 110,000 users.

## Running it

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/tmp/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

`PROCS` sets the number of processes, 6 by default.
