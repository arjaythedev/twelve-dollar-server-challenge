# Crystal + Kemal + SQLite

The feed API written the way a [Kemal](https://kemalcr.com/) app normally is: routes, one
`DB::Database` from crystal-db, `JSON::Builder` for responses, the `jwt` shard for tokens.
No custom I/O, no native tricks. The native, every-trick-in-the-book version is
[`crystal-epoll-faustinoaq`](../crystal-epoll-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1 (official tarball, sha256-checked by `install.sh`) |
| Framework | Kemal 1.15.0 (on Crystal's `HTTP::Server`, routes by radix 0.4.1) |
| SQLite driver | crystal-sqlite3 0.24.0 + crystal-db 0.15.0, on Ubuntu's libsqlite3 (3.45.1) |
| JWT | jwt 1.7.2 (HS256 through OpenSSL) |
| Nginx or direct | **Nginx**: the server listens on `127.0.0.1:3000` behind `bench/nginx.conf` |

All shard versions are pinned in `shard.yml` and locked in `shard.lock`; `build.sh` installs them
with `shards install --frozen`.

## Why Nginx

Crystal's `HTTP::Server` gives every connection its own fiber, with its own stack and I/O
buffers. That is about 100 KB of RSS per open keep-alive connection here, so serving tens of
thousands of browsing users directly would not fit in 2 GB. Behind Nginx, Kemal only sees
Nginx's small upstream pool.

## What is tuned

Not much, on purpose:

- `--release --mcpu x86-64-v3` build (fixed AVX2 baseline; native CPU detection produced
  SIGILL on some CI runners).
- One SQLite connection (`max_pool_size=1`). The server runs on one thread and SQLite has one
  writer, so a bigger pool only adds lock handoffs.
- Pragmas: WAL, `synchronous=NORMAL` (rule 6), foreign keys on, a 1 GB `mmap_size`, a 64 MB page
  cache, `busy_timeout`.
- Kemal's request log and `X-Powered-By` header are off, and it runs in production mode.
- Posts are serialized with `JSON::Builder` straight into the response IO, in the field order the
  tests expect.

Every request reads SQLite, and every JWT signature is verified on every request.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

On my machine (Ryzen 5 3600, server pinned to one core, wrk with 256 connections) it does about
9,500 req/s on `/feed` alone and 12,300 req/s on a mix of feed, single posts, likes and new posts.
