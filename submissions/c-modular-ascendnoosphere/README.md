# Ascend Noosphere

C11 / io_uring on Ubuntu 24.04 x86_64, static liburing 2.5, SQLite 3.54.1, yyjson 0.10.0, OpenSSL 3.0.13. Requires Linux 6.1+ with io_uring enabled; container syscall policies must allow it. Direct HTTP, no Nginx.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Prepared statements and direct B-tree reads reduce query overhead. SIMD parsing reduces scanning work. Multiple rings hold sockets in registered file tables. Bounded connection work and backpressure limit queueing and input growth.
