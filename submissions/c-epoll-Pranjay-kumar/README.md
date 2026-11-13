# C / epoll / SQLite — Pranjay-kumar

An independently written C11 HTTP/1.1 server using Linux epoll, SQLite's native API, json-c, and OpenSSL for HS256 JWT verification. It uses prepared statements and executes live SQL for every read. The implementation does not reuse the assembly submission's request handling or SQL routines.

## Build and run

On Ubuntu 24.04 x86_64, install dependencies and run the official test harness:

```sh
sudo bash submissions/c-epoll-Pranjay-kumar/install.sh
bash test/run.sh submissions/c-epoll-Pranjay-kumar
```

The server reads `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT`. It binds directly to `HOST:PORT` and does not require root to run. No database schema changes or kernel tuning are performed.

## Design

Each worker is an independent process with its own nonblocking `SO_REUSEPORT` listener, epoll loop, and SQLite connection. By default, the parent starts up to eight workers based on the CPUs visible to the process; `WORKERS` can set a value from 1 to 32. That keeps the one-vCPU challenge machine at one worker while allowing the same server to use multiple cores on larger machines. Each connection can use HTTP keep-alive; responses are sent with backpressure through writable epoll events. The server handles the challenge's health, feed, post lookup, create-post, and like routes. Feed and post responses use a small direct JSON writer; request parsing, token signature verification, and SQL access use the direct library APIs.

The organizer's complete test suite passed 42/42 checks on a fresh seed database in local Ubuntu 24.04 under WSL. Performance figures are not included; the official benchmark is the source of any leaderboard score.
