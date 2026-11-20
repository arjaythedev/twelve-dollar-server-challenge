# c-iouring-Elephantpanda6

A single-process C server for the five-endpoint feed API in [`SPEC.md`](../../SPEC.md).
One thread runs the event loop, one SQLite transaction covers each batch of requests,
and the hot path avoids syscalls, copies and allocations.

| | |
|---|---|
| Language | C11 (`-std=gnu11`), no framework |
| Build | GCC `-O3 -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -flto`, on-box PGO (`PGO=0 bash build.sh` skips training) |
| io_uring | raw syscalls, no liburing |
| Database | SQLite 3.54.1 amalgamation (SHA-256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`, verified by `build.sh`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## Architecture

- **Multi-ring direct descriptors.** Sockets are io_uring direct descriptors in each
  ring's fixed-file table, so `LimitNOFILE=65535` does not cap connections. Up to 16
  rings of 65,400 slots, created as the connection count grows; listeners use
  `SO_REUSEPORT`. Idle keep-alive connections are never closed.
- **Provided-buffer rings plus an ENOBUFS drain queue.** Receives come from 2 KB
  provided buffers. When a burst empties a ring and receives end with `ENOBUFS`, the
  connection goes on a queue and each pass issues one bounded `MSG_DONTWAIT` read per
  queued connection, re-arming the multishot receive once the socket is drained.
- **B-tree cursor reads with a SQL fallback.** `/feed` and `/posts/:id` walk SQLite's
  B-tree cursors through SQLite's pager and WAL and write JSON straight from the pages;
  unexpected schema or record layouts fall back to the prepared reference SQL. Writes
  are prepared SQL (`INSERT ... RETURNING`, `ON CONFLICT DO NOTHING`).
- **One transaction per batch, checkpoints off the loop.** Each pass commits once;
  responses are only sent after the commit returns. A nice-19 background thread runs
  PASSIVE checkpoints, writes are briefly held while the WAL tail is copied.
- **Zero-copy HTTP, JSON and HMAC.** Requests are parsed in place; responses are built
  body-first in a per-batch arena with headers written in front. JSON is emitted into
  the same buffer, and HS256 verification uses precomputed ipad/opad midstates.

## Build and run

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`build.sh` downloads and checksums the pinned amalgamation, trains `bin/train` against
a synthetic database, then rebuilds `bin/server` with the profile. Any training
failure falls back to the plain `-O3 -flto` build.

## Files

```
src/main.c             event loop and endpoint handlers
src/protocol.{c,h}     HTTP/1.1 parsing, HS256 JWT, JSON writers
src/sqlite_layer.{c,h} statements, B-tree cursor reads, WAL batching
src/uring_net.{c,h}    listener, connection rings, batch send arena
src/uring_rings.{c,h}  multi-ring direct descriptors, slot allocation
src/train.c            PGO training client
build.sh               SQLite download, verification, training, build
install.sh             Ubuntu 24.04 packages (run as root)
start.sh               foreground launch
```

## Credits

The architecture follows these submissions:

- **rust-iouring-kensac** (#264) — ring policy and checkpoint state
- **c-iouring-pavlyhalim** (#245) — WAL batching, page cache, batched lookups
- **c-iouring-jowinjestine** (#404) — read queue and batch structure
- **c-iouring-shivanshthapliyal** (#250) — SQLite integration
- **mojo-iouring-oh-meh** (#257) — io_uring event loop handling

All code here is new; any bugs are this submission's own.
