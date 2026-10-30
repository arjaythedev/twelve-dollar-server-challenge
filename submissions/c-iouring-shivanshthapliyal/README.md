# c-iouring-shivanshthapliyal

One thread of C on raw io_uring, with SQLite compiled into the same binary.

| | |
|---|---|
| Language | C (GCC 13 from Ubuntu 24.04, `-O2 -march=native -flto`) |
| Framework | None. A hand-written HTTP/1.1 server on io_uring |
| io_uring | liburing 2.5 (Ubuntu 24.04 package), features up to Linux 6.8 only |
| SQLite | 3.54.1 amalgamation, pinned by SHA-256, compiled in, not patched |
| Front | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box). No Nginx |

## Design

- **One thread, no locks.** The event loop, HTTP, JSON, JWT checks and SQLite all run on one thread. SQLite is built with `SQLITE_THREADSAFE=0`.
- **Rings.** Each io_uring ring has a sparse direct-descriptor table of `LimitNOFILE` slots (65,535). A new ring starts when the current one is close to full, up to 8 rings (about 520k connections). Ring flags: `SINGLE_ISSUER`, `DEFER_TASKRUN`, `SUBMIT_ALL`, `CQSIZE`.
- **Accept** is multishot with `IORING_FILE_INDEX_ALLOC`, on one ring at a time. It moves to the next ring 512 slots before the table is full, so the kernel never accepts and closes a connection for lack of a slot.
- **Receive** is multishot into a provided-buffer ring (1,024 × 2 KB per ring). The request is parsed in place and the buffer goes back at once. Idle connections hold no buffer. A connection record is 56 bytes.
- **Send** is one `SEND` per response, headers and body in one buffer. Two writes would hit Nagle and the 40 ms delayed ACK.
- **Batches.** All requests from one loop pass form a batch, capped at 1,024. Writes run first in one `BEGIN IMMEDIATE … COMMIT`. No write response leaves before its commit (rule 6). Reads then run in one read transaction, so they see the batch's writes. If the commit fails, every write in the batch gets a 500.
- **Keep-alive** connections stay open until the client closes them. The server never closes a healthy connection and never sheds load.

## SQLite

- Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `locking_mode=EXCLUSIVE`, `cache_size=-65536` (64 MiB), `mmap_size=0`, `temp_store=MEMORY`, `wal_autocheckpoint=0`, `journal_size_limit=64 MiB`.
- `locking_mode=EXCLUSIVE` keeps the WAL index in heap memory, so a read costs no shared-memory locks. It is still WAL with `synchronous=NORMAL`.
- **Checkpoints** run on the loop thread: `wal_checkpoint_v2(RESTART)` once the WAL passes 4,096 frames (16 MiB). The WAL stays small, so reads stay fast and the page cache stays small. A background checkpoint thread under constant writes never lets the WAL reset.
- All statements are prepared once (`SQLITE_PREPARE_PERSISTENT`) and reused.
- Build flags: `THREADSAFE=0 DEFAULT_MEMSTATUS=0 OMIT_DEPRECATED OMIT_SHARED_CACHE OMIT_LOAD_EXTENSION USE_ALLOCA DQS=0 DEFAULT_WAL_SYNCHRONOUS=1 MAX_EXPR_DEPTH=0 OMIT_PROGRESS_CALLBACK OMIT_AUTOINIT LIKE_DOESNT_MATCH_BLOBS`.

## Queries

- **Feed**, three plain SQL statements:
  1. `SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20` (covering index scan).
  2. `SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ? AND ? GROUP BY post_id`.
  3. `SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?`.

  Rows from steps 2 and 3 are matched to the 20 ids from step 1, and the output follows step 1's order. Rows outside the id set are skipped. If the ids span 256 or more, or a post has no author row, the server runs the reference query from SPEC.md instead. No assumption about dense ids or `created_at` order is needed for correctness.
- **Post:** the reference join with a correlated `count(*)`.
- **Like:** `SELECT 1 FROM posts WHERE id = ?` (404 if missing), then `INSERT … ON CONFLICT (user_id, post_id) DO NOTHING`; `sqlite3_changes()` picks 201 or 200.
- **Create:** `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`. `created_at` comes from the column default.

## HTTP, JSON, JWT

- Header names are matched case-insensitively. `Content-Length`, `Authorization`, `Connection: close` and `Expect: 100-continue` are handled. Pipelined requests are answered in order.
- Responses are compact JSON in the SPEC's key order, written straight into the send buffer. `Content-Length` is filled in after the body.
- JWT: the HMAC-SHA256 inner and outer pad states are computed once from `JWT_SECRET` at start (key setup). Every token's signature is still verified on every request. The header must say `HS256`. `exp` is required and checked. `sub` must be a positive integer string and `username` a string.
- Request bodies get a full JSON check. `body` is decoded, trimmed (JavaScript `trim()` whitespace) and counted in UTF-16 units, like `String.length`.

## Rule notes

- No caches of any kind: every request reads SQLite while it is served. No response, query-result or JWT caches.
- Plain SQL only: no SQLite internals, no B-tree cursors, no source patches.
- No load shedding, no error budgets, no memory-pressure tricks, nothing tuned to the load test.
- Configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.
