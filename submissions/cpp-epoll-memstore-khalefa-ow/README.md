# C++ + epoll + custom in-memory store: **no SQLite**

> **This server does not use SQLite.** It never opens `$SQLITE_PATH` while serving and doesn't link
> the SQLite library. Its data lives in its own in-memory store, made durable by its own WAL and
> snapshot files. SQLite is used exactly once: before the server starts, a separate `bin/import`
> program reads the seed `feed.db` and converts it into the store's format.
>
> That breaks rule 2 (the database is SQLite) and rule 5 (no in-memory copies of tables), so this
> is **not a valid entry** for the challenge. It shows how much of the per-request cost of the
> SQLite version, [`cpp-epoll_inmem-khalefa-ow`](https://github.com/khalefa-ow/twelve-dollar-server-challenge/tree/cpp-epoll_inmem-khalefa-ow/submissions/cpp-epoll_inmem-khalefa-ow),
> is SQLite.

| | |
|---|---|
| Language | C++17, built with Ubuntu's GCC (`build-essential`) at `-O2 -march=native` |
| Framework | none: a hand-written HTTP/1.1 server on Linux `epoll`, the same code as the SQLite version |
| **Database** | **Not SQLite.** A custom in-memory store (`src/store.hpp`) with its own WAL and snapshots in `$SQLITE_PATH.memstore/` |
| SQLite | **Only in `bin/import`**, a one-time `feed.db` → snapshot converter run by `start.sh`. `bin/server` doesn't link it |
| Crypto | OpenSSL `libcrypto` (Ubuntu's `libssl-dev`) for SHA-256 |
| JSON | hand-written strict RFC 8259 parser and writer |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself |

The HTTP server, JWT check and JSON parser are the same code as the SQLite version. Only the data
layer changed: SQLite is replaced by a store that holds the data in memory, laid out for these five
endpoints, and makes every write durable through its own WAL.

## Running it

```bash
sudo bash install.sh   # apt: build-essential, libssl-dev, curl
bash build.sh          # bin/server (no SQLite) and bin/import (feed.db -> snapshot)
SQLITE_PATH=... JWT_SECRET=... HOST=... PORT=... bash start.sh
bash test/run.sh submissions/cpp-epoll-memstore-khalefa-ow   # from the repo root: 42/42
```

`start.sh` runs `bin/import` first. It converts `feed.db` into the store's snapshot (about 2 s), or
does nothing if the store already came from this same `feed.db`. Then it starts `bin/server`, which
never opens SQLite. After that first conversion, `feed.db` is never read or written again: new posts
and likes go only to the store's own files.

| Env | Default | |
|---|---|---|
| `STORE_DIR` | `$SQLITE_PATH.memstore` | snapshot and WAL files |
| `WAL_SYNC` | `normal` | Like SQLite's WAL + `synchronous=NORMAL`. `full`: a write's response waits for its `fdatasync` (group commit on a sync thread), like `synchronous=FULL` |
| `SNAPSHOT_WAL_MB` | `64` | WAL size that triggers a snapshot |

## Memory layout ([`src/store.hpp`](src/store.hpp))

- **Posts: an array indexed by id.** Each entry holds the post's JSON already rendered up to
  `"like_count":`, plus the like count. `GET /posts/:id` is an array lookup and two appends.
- **Feed: post ids sorted by `(created_at, id)`.** The feed is the last 20. A new post is almost
  always the newest, so adding one is a `push_back`.
- **Likes: an open-addressing hash set.** Keys are `user << 32 | post` in 8 bytes, with linear
  probing at most 70% full. It only answers "already liked?"; the count lives on the post.
- **Authors:** usernames by user id. A post whose author isn't in `users` is never served, as the
  SQLite version's `JOIN` behaves.

## Durability

- **WAL**: every post and like is appended as a record: `{u32 length, u32 CRC-32C, payload}`.
  Each event-loop batch costs one `write()`, and **no response of a batch is sent until its records
  are written**. A batch with no writes skips the syscall.
- **Same guarantees as SQLite's WAL mode with `synchronous=NORMAL`** (the default):
  - A commit survives a process crash at once. It reaches the disk with the OS's writeback, so a
    power loss can drop the last commits but never corrupts the store.
  - As SQLite syncs the WAL before a checkpoint, the server `fdatasync`s a WAL file before a
    snapshot takes it over, and syncs replayed files at startup. Whatever survives a power loss is
    a prefix of the commits.
  - `WAL_SYNC=full` makes every acknowledged write durable, like `synchronous=FULL`, with **group
    commit on a sync thread**:
    - The loop still `write()`s each batch's records and keeps serving; it never waits for the disk.
    - The sync thread `fdatasync`s everything written so far. Commits that arrive during one sync
      share the next, so one sync can cover many batches.
    - Only the responses of connections that wrote are held until their records are synced. Read
      responses go out at once, and a connection's later responses queue behind its held ones.
    - Trade-off: a reader can see a like or post up to one sync (about 2 ms here) before it is
      durable. The writer is acknowledged only after it is.
- **Recovery**: the server maps the snapshot, replays the WAL files that come after it, and cuts
  off a torn tail: a cut-short record or a CRC mismatch was never acknowledged. Startup takes
  about 0.2–0.5 s.
- **Snapshots** (they compact the WAL): once the WAL reaches `SNAPSHOT_WAL_MB`, the server starts
  a new WAL file and `fork()`s.
  - The child closes the sockets it inherited and writes the frozen copy-on-write image to
    `snapshot.tmp`. Then it runs `fsync`, `rename` and `fsync(dir)`.
  - The parent keeps serving. When the child exits cleanly, the parent deletes the WAL files the
    snapshot covers.
  - Rendered JSON for snapshot posts is served straight from the mapped snapshot file.
- Tested: `kill -9` of the whole process group with writes in the WAL, with a snapshot in
  progress, and after a snapshot finished, plus a torn record appended to the WAL. Each time the
  feed and the like counts after restart were identical.

## Cost per request vs. the SQLite v2 server

`comparison/cpu-cost.sh`, same machine, server pinned to one core. The client tops out at about
12k req/s, so these are CPU costs, not maximum throughput.

| endpoint | SQLite v2 user µs | memstore user µs | SQLite v2 total µs | memstore total µs |
|---|---|---|---|---|
| health | 2.0 | 1.6 | 15.2 | 16.8 |
| post | 10.1 | 3.0 | 25.3 | 17.4 |
| feed | 47.0 | 2.8 | 65.6 | 18.8 |
| like | 35.0 | 4.5 | 80.1 | 18.5 |
| create | 15.2 | 5.5 | 35.8 | 21.4 |
| mix | 21.0 | 2.4 | 36.6 | 12.3 |

These numbers were measured with the first `WAL_SYNC=full`, which blocked the loop on every
sync (the group-commit version came later). With the store out of the way, what remains is
almost all kernel networking time (`sys`, about 15 µs per request). Snapshot children are separate
processes, so their CPU doesn't show in these numbers (no snapshot ran during the 8 s windows).

Memory: about 112 MB anonymous (post array, like set, new posts) plus the 174 MB snapshot mapping,
which the kernel can reclaim.

## Limits

- `likes.created_at` isn't stored, since no endpoint returns it.
- With `WAL_SYNC=full`, a write still takes one `fdatasync` (about 2–4 ms median on the test
  machine) before its response. Group commit removes that wait from reads, not from writes.

## Group commit vs. blocking sync (`WAL_SYNC=full`)

k6 at a fixed request rate with `bench/load.js`'s mix, server pinned to one core. Latency in ms.

| variant | rate (req/s) | read p50 | write p50 | server CPU |
|---|---|---|---|---|
| full, blocking (before) | 3,000 | 0.91 | 2.94 | 14% |
| full, group commit | 3,000 | **0.35** | 3.87 | 31% |
| normal | 3,000 | 0.24 | 0.27 | 19% |
| full, blocking (before) | 8,000 | 4.28 | 4.63 | 19% |
| full, group commit | 8,000 | **0.37** | 3.74 | 39% |
| normal | 8,000 | 0.62 | 0.65 | 35% |

Reads no longer wait for the disk. With the blocking sync, read p50 grew with load (4.3 ms at
8,000 req/s); with group commit it stays at the network floor, and writes keep paying about one
sync. Group commit costs more CPU here, since the thread syncs as soon as anything is written.
p95/p99 on this WSL2 laptop were dominated by stalls that hit every variant, even `normal`
(tens of ms), so they are left out.
