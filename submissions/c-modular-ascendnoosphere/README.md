# Ascend Noosphere

C11 / io_uring on Ubuntu 24.04 x86_64, static liburing 2.5, SQLite 3.54.1, yyjson 0.10.0, OpenSSL 3.0.13. Requires Linux 6.1+ with io_uring enabled; container syscall policies must allow it. Direct HTTP, no Nginx.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Reads use SQLite's B-tree cursor API with a prepared SQL fallback. SIMD parsing reduces scanning work. Network completions are drained before request handling, with bounded connection work and backpressure. Passive WAL checkpoints run on a separate SQLite connection and thread, with automatic checkpoints as a fallback. Sockets are accepted directly into registered file tables, which grow across rings as connections arrive.
