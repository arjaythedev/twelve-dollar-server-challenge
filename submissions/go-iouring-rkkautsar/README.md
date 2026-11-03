# Go io_uring + direct SQLite

**Language:** Go 1.27.1
**Transport:** custom HTTP/1.1 parser over raw Linux io_uring; `net/http` is used only for status constants and the optional pprof listener
**Driver:** direct cgo against the vendored SQLite 3.46.1 amalgamation (`sqlite/sqlite3.c`, `sqlite3.h`), built with `SQLITE_THREADSAFE=2` and `SQLITE_OPEN_NOMUTEX`. The two fixed read shapes also use SQLite's in-process B-tree cursors through a one-translation-unit C shim.
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx

All five endpoints read SQLite while the request is being served. There is no response
cache, query-result cache, in-memory table copy, or remembered JWT verification. With
`READ_COALESCE=1` (default), groups of concurrent reads share only their in-flight SQLite
result, and the group is removed as soon as that read completes. `READ_COALESCE=0` disables
read sharing entirely: every feed/post request executes its own SQLite query. Prepared
statements cache only query plans, as allowed by rule 5.

## Architecture

- **Raw io_uring is the default.** The default layout starts four `SO_REUSEPORT` listener
  shards. Each shard owns an io_uring, a 65,000-entry registered file table (260,000 slots
  across shards), its HTTP parser, and its output queues. Accepted sockets use
  `IORING_FILE_INDEX_ALLOC`, so accepted connections do not consume the process
  `RLIMIT_NOFILE` budget. Sparse file-table registration is requested first; the
  registration size must fit inside the hard `RLIMIT_NOFILE`, otherwise Linux returns
  `EINVAL` and the normal fallback hits `EMFILE`. The 65,000 size is deliberately below
  the challenge's 65,535 hard limit. If ring setup still fails, the process falls back to
  the Go connection loop; that loop backs off on `EMFILE`/`ENFILE` instead of spinning.
- **Read worker per shard, one shared writer.** The default layout has one blocking read
  worker per shard for feed and post requests, and one writer shared by all shards for
  create/like batches. There is no goroutine per connection and no per-request transport
  goroutine. `MUX_LAYOUT=mixed` instead gives each shard one worker for feed, post, and
  write jobs; `MUX_LAYOUT=feedpost` gives each shard separate feed and post workers. With
  coalescing enabled, feed requests share one in-flight read, post IDs are served from
  the B-tree path, and the writer batches writes into one transaction. All already-queued
  write jobs merge into the next transaction; no artificial timer is added when the queue
  is empty. The SQL fallback
  batches post IDs into one `IN` query. With
  `READ_COALESCE=0`, reads go through the generic single-request handler instead, so no
  read result is shared between requests.
- **Durable, batched writes.** The raw writer uses `BEGIN IMMEDIATE`/`COMMIT` and only
  queues a 201/200 response after the commit succeeds. SQLite runs WAL with
  `synchronous=NORMAL`; the schema is unchanged and no checkpoint or durability pragma is
  weakened. The Go fallback group-commits writes every `WRITE_BATCH_MS` (default 100 ms),
  and every response waits for its commit. Liked posts are resolved for the whole batch
  with one `IN` query; new likes are inserted with one JSON-driven multi-row
  `INSERT ... json_each ... RETURNING` statement. Creates use the same shape; the
  returned ids are sorted before they are paired with requests, so each response gets
  its own generated id and timestamp. The SQLite WAL stays on its default 1,000-page
  automatic checkpoint; moving checkpoints to a helper or disabling them was measured
  and worse on this box.
- **Multishot transport.** Accept and receive are multishot io_uring operations. Receives
  use one registered provided-buffer ring of 32,768 1 KiB buffers per shard.
  Each ring uses a 16,384-entry completion queue. Four rings are the default: they cover
  the 260,000 direct-connection slots without exhausting the io_uring locked-memory
  budget, while five rings measured slower at the same load.
  A receive is re-armed only when `IORING_CQE_F_MORE` is absent or the ring reports
  `-ENOBUFS`; consumed buffers are returned by advancing the buffer-ring tail. There is no
  epoll descriptor and no eventfd: the worker wakes the event loop through a pipe read SQE.
- **Buffer lifetime.** Input and output slices are released when empty. The raw path builds
  complete HTTP packets, so a normal response uses one send. `SEND_ZC` was measured and
  made throughput worse, so plain `IORING_OP_SEND` is used.
- **Parser.** The request line and only the headers this API needs (`Authorization`,
  `Content-Length`, `Connection`) are parsed into a small struct. The tested surface is
  Content-Length requests from curl/k6; chunked bodies and unusual methods are outside it.
- **B-tree reads.** `/feed` and `GET /posts/:id` use SQLite's own B-tree cursors,
  not the SQL parser/VM. `btree.c` includes the amalgamation in one C translation unit,
  exposing `feed_btree()` and `posts_btree()` to Go through cgo. A reader context per read
  connection owns the cursors, key infos and like-seek key; each request still starts and
  commits its own read transaction. Feed opens `posts_created_at_id_idx`, walks its DESC
  order, and for each row reads the `posts` table record, the `users` record, and counts
  `likes_post_id_idx` entries. The post batch opens one transaction and one cursor set for
  all ids in a coalesced batch. A fixed-schema record decoder reads only the needed fields
  instead of constructing generic SQLite `Mem` records. The shim checks root pages at
  startup and falls back to the SQL path on any schema/record error. It does not retain
  row data across requests.
- **SQLite queries.** The B-tree path is a fixed-shape accelerator; the SQL path remains
  for fallback and writes. `/feed` and single-post SQL use `json_object`/
  `json_group_array`; `loadPosts` prepares one `IN` query per batch; statements do not
  cache rows. `like_count` is counted from `likes` in the request. Read connections use
  `cache_size=-16384` (16 MiB) and `mmap_size=268435456` (256 MiB); these are SQLite page
  cache and mmap, not application caches.
- **JWT.** HS256 is verified directly with `crypto/hmac`; only HS256 is accepted, `exp` is
  checked, and the signature is verified for every request. There is no token cache.
- **Fallback.** `URING=0` selects the Go `net`-based connection loop and read-coalescing
  path. It is available for diagnosis and for kernels where the raw path cannot initialize;
  the raw path is what is benchmarked.

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required by the challenge | SQLite database file |
| `JWT_SECRET` | required by the challenge | HS256 signing secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `URING` | unset/`1` | `0` selects the Go fallback |
| `SHARDS` | `4` | io_uring listener shards (four 65k file tables) |
| `MUX_LAYOUT` | `read` | `read`: one read worker per shard plus one shared writer; `mixed`: one feed/post/write worker per shard; `feedpost`: one feed and one post worker per shard plus one shared writer |
| `READ_CONNS` | `2` | SQLite read connections |
| `BTREE` | `1` | `0` disables the B-tree read path and uses SQL only |
| `STATS` | unset | `1` logs io_uring, B-tree, write and process CPU counters every 10 s |
| `READ_COALESCE` | `1` | `0` disables in-flight read sharing |
| `READ_BATCH_US` | `500` | Go fallback read window |
| `WRITE_BATCH_MS` | `100` | Go fallback write commit window |
| `PPROF_ADDR` | unset | Optional pprof listener |

## Local measurements

Fresh seed database, direct on one CPU, 80 s runs. The client container count matters:
16 containers × 10,000 VUs is 160k, 16 × 12,500 is 200k.

| Run | Server CPU | Peak RSS | Client p95 | Result |
|---|---:|---:|---:|---|
| **160k, 16 clients** | 86.4% | 1,034 MiB | 420 ms | pass |
| 200k, 16 clients | 93.7-96.6% | 1.20-1.26 GiB | 0.7-2.2 s post, 2.6-3.7 s like | fail |

The 160k result is close to the machine's local noise floor: a second identical
configuration can fail on one tail event. 200k is consistently outside the target.
At 160k, `STATS=1` showed:

```text
feed_btree calls:   23,204   total 1,509 ms
posts_btree calls:  12,411   total 1,612 ms
write batches:       6,083   total 32,834 ms
create:             10,459   total 1,740 ms
likes:              77,146   total 10,473 ms
commit:                      18,602 ms
process user ticks:    955
process system ticks: 5,942
```

The B-tree reads are effectively free; the writer and kernel time dominate. Writes are
only ~7% of requests but are serialized through one SQLite writer. At 200k the writer
queue cannot keep up and the write tail sets p95.

`PRAGMA synchronous=OFF` and `journal_mode=OFF/MEMORY` are forbidden by rule 6 and were
not used. Off-path WAL checkpoint helpers, write gating, larger checkpoint intervals,
and `mmap_size=0` were measured; none beat the default. Four rings beat five at this
load.

## Scripts

- `install.sh` — Go 1.27.1 and `build-essential` (cgo); SQLite is vendored, so no system
  SQLite development package is installed.
- `build.sh` — cgo build of the vendored amalgamation, trimpath and stripped, to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
- `dev/` — optional local Docker/k6 load scripts used during development; not used by CI.

## Verification

`bash test/run.sh submissions/go-iouring-rkkautsar` passes all 42 checks on a fresh seed
database copy.
