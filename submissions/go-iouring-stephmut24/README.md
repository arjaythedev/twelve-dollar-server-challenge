# Go + io_uring + SQLite B-tree reads (`go-iouring-mutarushwa`)

Target: **~230k** concurrent users on the $12 / 1 vCPU / 2 GB box (same band as
`go-iouring-rkkautsar` #260), without read coalescing (rule 5 safe).

| | |
|---|---|
| Language | Go 1.27.2 (official tarball) + C11 (gcc from `build-essential`) |
| I/O (Linux) | io_uring via liburing (registered descriptors, multishot accept/recv, provided buffers); epoll fallback (`SERVER_IO=epoll`) |
| I/O (Darwin CI/dev) | `net/http` (functional tests only) |
| SQLite | 3.54.0 amalgamation, unity-included in `fastdb.c`, SHA-256 pinned in `build.sh` |
| Reads | SQLite **B-tree cursors** for `GET /feed` and `GET /posts/:id` (SQL fallback). Set `BTREE=0` to force SQL |
| Writes | SQL `INSERT…` + group commit per event-loop batch; responses after `COMMIT` (rule 6) |
| **Nginx or direct** | **Direct** on `0.0.0.0:80` |

## Credits

- B-tree read plans and record decoding adapted from
  [`c-iouring-pavlyhalim` #245](../c-iouring-pavlyhalim/src/btree.h) (`btree.h` here), per Arjay’s
  2026-11-04 ruling that credited helper reuse is fine.
- Event-loop / io_uring registered-FD shape inspired by `go-epoll-champ3oy` and
  `go-iouring-pmatteo` / `rust-iouring-kensac` notes (multi-ring, buffer ring, checkpoint off-loop).

The amalgamation is **unmodified**. We never write `BtCursor` private fields (ruling 8).
There is **no** coalescing of independent `GET /feed` / `GET /posts/:id` requests into one query.

## Running

```bash
sudo bash install.sh   # Ubuntu 24.04: toolchain, liburing, Go
bash build.sh          # downloads SQLite amalgamation, builds bin/server
# optional PGO on the box:
#   SQLITE_PATH=/tmp/feed.db bash pgo/train.sh && PGO=1 bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Architecture

1. **One OS thread** (`GOMAXPROCS=1`, `LockOSThread` on Linux) owns the event loop and the main SQLite connection (`THREADSAFE=2` for the checkpoint thread).
2. **io_uring**: client sockets are **registered descriptors** (not process FDs), so capacity is `rings × RLIMIT_NOFILE`. Soft shedding sends `Connection: close` near table capacity.
3. **Group commit**: likes/posts collected in one pass share `BEGIN IMMEDIATE … COMMIT`; no 201 until commit returns.
4. **WAL checkpoints** on a second connection/thread (`wal_autocheckpoint=0`).
5. **B-tree feed**: walk `posts_created_at_id_idx`, range-count likes, seek posts/users; fall back to the reference SQL when layout/ids don’t fit.
6. **Zero-ish alloc hot path** on Linux: arenas, hand-rolled HTTP/JWT; C renders JSON bodies.

## Pragmas

`journal_mode=WAL`, `synchronous=NORMAL`, `mmap_size=0`, 64 MiB `cache_size`, `temp_store=MEMORY`, `wal_autocheckpoint=0`.

## License

MIT, under the repo’s [license](../../LICENSE).
