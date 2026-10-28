# PHP + Swoole

| | |
|---|---|
| Language | PHP 8.3.6 (Ubuntu 24.04's `php8.3-cli` package), opcache + tracing JIT |
| Framework | Swoole 6.2.3 `Swoole\Http\Server`, built from the source tarball in `install.sh` (sha256 pinned) |
| SQLite driver | `pdo_sqlite` (`php8.3-sqlite3`), linked to Ubuntu's SQLite 3.45.1 |
| Dependencies | None beyond PHP and Swoole. No Composer packages |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: php8.3 cli/dev/sqlite3/mbstring/opcache; compiles and installs swoole.so
bash build.sh          # nothing to compile; lints server.php
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` doesn't enable Swoole globally. `start.sh` loads it with `-d extension=swoole`.

## Optimizations, and why

- **One process, one SQLite connection, synchronous handlers.** The server uses `SWOOLE_BASE` mode with
  `worker_num => 1` and `enable_coroutine => false`. Swoole's C reactor (epoll) handles the sockets and
  calls the PHP handler inline. On one vCPU, more workers would only add context switches and SQLite
  lock contention. Every query is an index or primary-key lookup that takes well under a millisecond,
  so there is nothing to gain from yielding to coroutines. With a single writer, writes never wait on
  SQLite locks.
- **Direct instead of Nginx.** The epoll reactor holds tens of thousands of idle keep-alive connections
  cheaply, so Nginx's 14–20% CPU stays available for PHP. `max_connection` is raised to the open-file
  limit and the listen backlog to 4096. Swoole doesn't time out idle keep-alive connections, because no
  heartbeat is configured.
- **`memory_limit=-1`.** In `SWOOLE_BASE` mode Swoole gives every connection a 64 KiB receive buffer from
  PHP's allocator and keeps it for the connection's lifetime, so PHP counts 64 KiB per open connection
  (about 1.4 GB at 20,000 connections). Only the pages a request touches become resident, so real RSS
  was 840 MB at 20,000 connections. A finite `memory_limit` would kill the worker on bookkeeping
  alone; the 2 GB of the box is the real limit.
- **Lean request handling.** POST body, cookie and multipart parsing and HTTP compression are off. Routing
  is a few string comparisons, with no router or framework. Success bodies for likes and health are
  assembled as strings, and everything else goes through `json_encode` (in C) with unescaped slashes
  and unicode.
- **Prepared statements are reused** for every query. They cache the query plan, not the data (rule 5).
- **SQL**: the reference queries. A like is one statement,
  `INSERT ... SELECT ?, ? WHERE EXISTS (post) ON CONFLICT DO NOTHING`. The post-existence check runs only
  when nothing was inserted, to tell "already liked" from "no such post".
- **Durability**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), with autocommit. The `INSERT ...
  RETURNING` statement is stepped to completion (`fetchAll`) before the 201 is sent, so the row is
  committed first. Other pragmas: 64 MiB page cache, 256 MiB `mmap`, in-memory temp store.
- **JWTs are verified on every request** with `hash_hmac` and a constant-time `hash_equals`. Only HS256 is
  accepted, `exp` is required and checked, base64url segments must be canonical, and nothing is cached.
- **opcache + tracing JIT** for the long-running CLI process.
- Post bodies are trimmed of the same whitespace as JavaScript's `String.prototype.trim()` (including NBSP
  and other Unicode spaces), like the reference implementations. The 500-character limit uses `mb_strlen`,
  which counts characters the same way SQLite's `length(body)` CHECK does.

## Results

Local runs: an Ubuntu 24.04 container limited to 1 CPU (Apple Silicon) and 2 GB, `nofile` 65535, with k6
in a separate container.

`bench/load.js` with the default 60 s ramp and 5-minute hold, following the scoring protocol: a 1,000-user
warm-up, then doubling.

| Users | Errors | p95 | p99 | Peak CPU | Peak RSS |
|---|---|---|---|---|---|
| 2,500 | 0 | 1.03 ms | 1.56 ms | 8% | 199 MB |
| 5,000 | 0 | 0.95 ms | 1.50 ms | 13% | 374 MB |
| 10,000 | 0 | 0.96 ms | 1.64 ms | 19% | 727 MB |
| 20,000 | 0 | 1.01 ms | 4.39 ms | 25% | 839 MB |

40,000 couldn't be measured locally: the k6 container ran out of memory at about 22,000 users, while the
server stayed up. Memory per open connection, not CPU, is this server's limit. Expect lower numbers on
the droplet, which has a slower vCPU, shares its 2 GB with the OS and handles all network processing itself.

A closed-loop run (64 VUs, `/feed` + random `/posts/:id`, no think time) served 54k req/s with p99 2.3 ms.

## License

MIT, under the repo's [license](../../LICENSE).
