# C / epoll / SQLite — Pranjay-kumar

An independently written C11 HTTP/1.1 server using Linux epoll, SQLite's native API, json-c, and OpenSSL for HS256 JWT verification. It uses prepared statements and executes live SQL for every read. The implementation does not reuse the assembly submission's request handling or SQL routines.

## Build and run

On Ubuntu 24.04 x86_64, install dependencies and run the official test harness:

```sh
sudo bash submissions/c-epoll-Pranjay-kumar/install.sh
bash test/run.sh submissions/c-epoll-Pranjay-kumar
```

**Networking mode: direct on `0.0.0.0:80`, with no Nginx.** This is the mode used for the challenge benchmark. Port 80 requires root or `CAP_NET_BIND_SERVICE`; `HOST` and `PORT` can override the defaults. The server also reads `SQLITE_PATH` and `JWT_SECRET`. It raises its `RLIMIT_NOFILE` soft limit to the existing hard limit before starting workers, then sizes each worker's client table from that limit (capped at 131,072 descriptors). No database schema changes or kernel tuning are performed.

## Design

Each worker is an independent process with its own nonblocking `SO_REUSEPORT` listener, epoll loop, and SQLite connection. By default, the parent starts up to eight workers based on the CPUs visible to the process; `WORKERS` can set a value from 1 to 32. That keeps the one-vCPU challenge machine at one worker while allowing the same server to use multiple cores on larger machines. Each connection can use HTTP keep-alive; responses are sent with backpressure through writable epoll events. The server handles the challenge's health, feed, post lookup, create-post, and like routes. Feed and post responses use a direct JSON writer to avoid per-field JSON objects, and `sendmsg` writes the header and body as separate buffers to avoid assembling a second response copy. Request parsing, token signature verification, and SQL access use the direct library APIs.

The organizer's complete test suite passed 42/42 checks on a fresh seed database in local Ubuntu 24.04 under WSL. Performance figures are not included; the official benchmark is the source of any leaderboard score.
