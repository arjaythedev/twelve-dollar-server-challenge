# C, no framework: io_uring + SQLite B-tree reads

One thread drives everything: an io_uring event loop, an HTTP/1.1 parser, JSON, HS256
verification, and SQLite (the pinned, unmodified amalgamation, linked in statically). A second,
nice-19 thread runs WAL checkpoints and nothing else.

| | |
|---|---|
| Language | C11 (gcc), no framework, no runtime |
| SQLite | 3.54.1 amalgamation, sha256-checked by `build.sh`, compiled with `-O3 -march=native` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings) |

This entry is a C implementation derived from two credited sources. The serving side — io_uring
transport, HTTP/1.1 parser, JSON, JWT, event loop, connection table, response writer and the
feed/post plans here — is written for this entry. The SQLite layer in `src/db.c` (open/pragmas,
per-pass transaction, group commit, prepared write statements, and the checkpoint/bulk-hold state
machine) is a C adaptation of the author's `rust-iouring-fexaop` entry, which builds on
**rust-iouring-kensac #264** (MIT); #264's durability and batching design is credited there and
here. The B-tree record decoder, cursor steps, index seek and the batched rowid descent are
adapted from **c-iouring-pavlyhalim #245** (MIT), credited in `src/db.c`. Credited reuse is
allowed by the challenge's copies rulings; the transport and request logic are not a copy of
another entry.

## Connections

- Accepted sockets are io_uring **direct descriptors**. A ring's file table holds up to
  `RLIMIT_NOFILE` (65,535 on the benchmark box), so the server starts with one ring and adds
  another when the accepting ring is 1,024 slots from full, up to 16 rings. The port has eight
  `SO_REUSEPORT` listeners, each with its own accept queue; single-shot accepts are kept 512 in
  flight and re-armed after a pass.
- One **multishot receive** per connection into a provided-buffer ring of 4,096 × 1 KiB. Idle
  connections hold no receive buffer. After a burst that empties the ring (`ENOBUFS`), the
  connection goes on a FIFO read queue and is drained with single-shot `MSG_DONTWAIT` reads, never
  more per ring than it has buffers; a short read re-arms the multishot.
- One `SEND` per connection at a time. Output buffers come from a process-wide pool and go back
  once the send completes, so idle connections hold no heap memory. Closing is a `SHUTDOWN`
  hard-linked to a `CLOSE` of the direct descriptor (through the ring's file table).
- At most 2,048 requests are answered per event-loop pass. A connection whose request bytes were
  not reached (or that is mid-send) keeps them in a compact 512-byte-and-growing pending buffer
  and goes on a fixed-size ready queue that the next pass drains before and after reads. This
  bounds parse+SQLite work per pass so accepts, sends, COMMIT and checkpoint work cannot be
  starved at 300k+ connections; an EOF close waits until a deferred complete request is answered.
- Accepted sockets inherit `TCP_NODELAY` and `TCP_CONGESTION=reno` from the listener (a per-socket
  option inside the process; no kernel parameter). The executable's and libc's read-only code
  mappings are `mlock`ed up to 2 MiB.
- One thread, `SINGLE_ISSUER | DEFER_TASKRUN`. Each pass submits every ring and waits on ring 0 for
  256 completions or 1 ms; with work queued it does not wait. One SQLite transaction per pass:
  reads share its snapshot, writes are group-committed, and no response is sent before COMMIT.

## SQLite

- WAL + `synchronous=NORMAL`, opened through the `unix-excl` VFS (the WAL index is heap memory, no
  `-shm` file, no per-transaction fcntl locks). `wal_autocheckpoint=0`; a nice-19 thread runs
  `PASSIVE` checkpoints. **The event loop never checkpoints.** Past 4,096 WAL frames the thread
  copies the log back; then writes are held (reads continue) for at most 250 ms while it copies
  the frames written meanwhile, so the next write restarts the log. Keeping the log short matters:
  the wal-index hash is merged when a frame segment is appended, so an ever-growing log makes every
  commit more expensive. If the thread cannot keep up even so, the WAL grows and reads keep finding
  the newest frame through the wal-index; a blocking checkpoint on the only CPU is what turns a
  checkpoint backlog into a multi-second p99.
- Reads walk SQLite's own B-tree cursors (`csrc` is the unmodified amalgamation in `src/db.c`),
  inside the pass's transaction. Rust-style value copies and the bytecode engine are skipped; the
  record bytes are decoded directly.
- **`GET /feed`:** if `created_at` never decreases with rowid, `ORDER BY created_at DESC, id DESC`
  is reverse rowid order and the feed is the last 20 rows of the `posts` B-tree: one
  `sqlite3BtreeLast` and 20 `sqlite3BtreePrevious` steps, then 20 author rowid seeks and one range
  walk of `likes_post_id_idx`. The 20 authors are looked up in one batched rowid descent (keys on
  the same page fetch it once, and their cache misses overlap; adapted from c-iouring-pavlyhalim
  #245, credited). The invariant is verified by a full scan of the actual table at
  startup; every `INSERT … RETURNING` compares its timestamp with the previous one and switches the
  plan off for good if it ever goes backwards. If the check fails, or a post's author row is
  missing (where the reference JOIN would drop it), the entry uses the index plan / reference SQL.
  Every request reads its rows from SQLite while it is served; the only state kept is one boolean
  and the previous 24-byte timestamp.
- **`GET /posts/:id`:** a rowid seek in `posts`, a rowid seek in `users`, and one range count in
  `likes_post_id_idx`. The row is decoded in place: the response is built from SQLite's page bytes
  before the cursors move, with no intermediate copy of the body, timestamp or username.
- **Writes:** `INSERT … RETURNING id, created_at` and `INSERT … ON CONFLICT DO NOTHING`;
  `sqlite3_changes` separates a new like from a repeat. A like's 404 check is a B-tree seek.
- Responses are written through a raw pointer after one capacity check per post object and one
  per response head; the field lengths are known, so a 6x bound on the escaped text is exact.
- The startup scan runs on a throwaway connection with a 1 MiB page cache, so the serving
  connection's cache starts empty and only ever holds the hot set (peak RSS stayed near 11 MB at
  128 users); it then drops the table's clean page-cache pages with `posix_fadvise(DONTNEED)` on
  the server's own `$SQLITE_PATH` only, so they are not charged to the server's cgroup (the same
  technique mojo-iouring-oh-meh uses; the odin #139 flag was about dropping the whole box's
  cache, not its own database file).

## Correctness

- `bash test/run.sh submissions/c-iouring-fexaop`: **42/42**.
- Non-monotonic database: a copy of the seed with `created_at` moved to 2027 on post 1000 makes the
  startup check fail; `/feed` then uses the index plan and matches the reference SQL byte for byte.
- After 200 `POST /posts` inserts the feed still matches the reference SQL byte for byte and the
  insert guard keeps the tail plan on.
- Ring spilling and slot reuse: with 1,024-slot rings, 2,500 idle connections create 3 rings and
  the last connection is served; 3,000 connect/health/close cycles all succeed (direct-descriptor
  close frees the slot).

## Benchmark notes

The server retries `io_uring_setup` and halves the CQ ring (65,536 → 8,192) if the kernel returns
`ENOMEM` under memory fragmentation, shrinks the direct-descriptor table (the hard limit → 65,535 →
16,384 → 4,096) if registering it fails under a small `RLIMIT_MEMLOCK`, and falls back to
2,048/1,024/512/256 receive buffers per ring if the 4,096-buffer ring does not fit. On the
benchmark box none of these fallbacks should be needed.

See `BENCH.md` for the measurements taken on the development server.

## Files

| | |
|---|---|
| `src/server.c` | rings, accept/receive/send/close, read/ready queues, per-pass answer budget, ring spilling, buffer pool |
| `src/app.c` | routes, handlers, responses, output buffers |
| `src/db.c` | SQLite open/pragmas, record decoding, feed/post plans, writes, checkpoint thread |
| `src/http.c`, `src/json.c`, `src/jwt.c`, `src/sha256.c` | request parser, JSON, HS256, SHA-256/HMAC |
| `src/*.h` | interfaces |
| `install.sh`, `build.sh`, `start.sh` | build/run |

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
