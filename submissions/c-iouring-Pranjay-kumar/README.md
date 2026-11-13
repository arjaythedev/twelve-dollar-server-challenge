# C / io_uring / SQLite — Pranjay-kumar

An independent GNU C11 server using raw `liburing` operations, SQLite's native API, json-c, and OpenSSL HS256 verification. This is a separate io_uring implementation from the earlier C/epoll entry.

## Run mode

**Direct on `0.0.0.0:80`, without Nginx.** Port 80 needs root or `CAP_NET_BIND_SERVICE`. The server reads `HOST`, `PORT`, `SQLITE_PATH`, and `JWT_SECRET`; the listener defaults to `0.0.0.0:80`.

```sh
sudo bash submissions/c-iouring-Pranjay-kumar/install.sh
bash test/run.sh submissions/c-iouring-Pranjay-kumar
```

## Design

- Eight independent processes bind the same port with `SO_REUSEPORT` by default. Each process has its own io_uring, SQLite connection, descriptor table, and accept queue. `WORKERS` sets a value from 1 to 16. The parent raises the soft `RLIMIT_NOFILE` to the existing hard limit before forking.
- An idle keep-alive connection retains compact metadata and a one-shot io_uring poll. A fixed pool of 2,048 request buffers per worker is assigned only when a connection becomes readable, then returned after its request is handled. Idle connections do not reserve a request buffer.
- SQL queries are prepared once and executed live for each request. WAL with `synchronous=NORMAL` is enabled; there are no response or database-result caches, schema changes, or kernel tunings.
- HTTP receives and sends are asynchronous io_uring operations. JSON and JWT use json-c and OpenSSL.

The official suite passed 42/42 on a fresh seed database under Ubuntu 24.04 in WSL. A short local challenge-model run at 20,000 VUs completed with 0% failed requests (p95 0.81 ms, p99 3.46 ms); this is not the scored VM or the official five-minute hold. No leaderboard score is claimed.
