# Ada on a $12 server

We wanted to bring a less usual language to the challenge, so we picked Ada
and started squeezing. That turned into a small HTTP server, some time spent
chasing allocations, and a lot more attention to idle sockets than expected.

The implementation uses **Ada 2022**, with no web framework: HTTP and JSON are
written in Ada, sockets use Linux libc bindings, SQLite uses direct C API
bindings, and OpenSSL handles HS256 verification. It targets Ubuntu 24.04 x86_64
and **serves directly on `0.0.0.0:80`**.

## What we squeezed

- Replaced `poll` with a single nonblocking **epoll** loop, so readiness handling
  doesn't have to walk every open connection. Free slots are reused, and
  generation tags keep stale events from reaching a new connection.
- Went direct to save the proxy overhead. Keep-alive and pipelining stay in the
  Ada server, with a small request budget per connection to keep things moving.
- Made input buffers grow only when needed and freed them when empty, so idle
  connections don't sit on a large buffer each.
- Copied ordinary JSON string runs in bulk and reused SQLite prepared statements
  to cut down on allocation and query preparation work.

SQLite uses WAL with `synchronous=NORMAL`. Writes commit before success responses,
the schema and indexes are unchanged, and each request reads current data.
JWT signatures are verified on every authenticated request.

## Build and run

Package versions are pinned in [versions.env](versions.env):

| Dependency | Ubuntu package version |
| --- | --- |
| GNAT/GCC and libgnat | 13.3.0-6ubuntu2~24.04.1 |
| SQLite | 3.45.1-1ubuntu2.8 |
| OpenSSL | 3.0.13-0ubuntu3.16 |

From this directory, install once as root, then build as a normal user:

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/absolute/path/to/private-feed-copy.db \
JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` runs in the foreground. Use a writable copy of the seed database,
with permission to create its WAL and SHM files. The challenge service provides
`CAP_NET_BIND_SERVICE` for port 80. For an unprivileged local run, use
`HOST=127.0.0.1 PORT=3000`; these are also the defaults.
