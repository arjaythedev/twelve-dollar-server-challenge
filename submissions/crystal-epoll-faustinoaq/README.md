# Crystal, no framework: io_uring + SQLite, no GC

The whole server is Crystal with the garbage collector compiled out (`-Dgc_none`): its own
io_uring layer through raw syscalls (epoll as a fallback), HTTP/1.1 parser, router, JSON
validator/writer, base64url and JWT check. SQLite is the pinned amalgamation, built with PGO and
linked in statically. libcrypto is used only for its SHA-256 block function.

The Kemal version of the same API is [`crystal-kemal-faustinoaq`](../crystal-kemal-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1 (official tarball, sha256-checked by `install.sh`), no shards |
| Framework | none: one thread, io_uring via raw `io_uring_setup`/`enter`/`register` syscalls |
| SQLite | 3.54.1 amalgamation, sha256-checked, built by `build.sh` with `-O3 -march=x86-64-v3` + PGO |
| Crypto | OpenSSL 3 libcrypto (Ubuntu package): `SHA256_Init/Update/Final` for HMAC-SHA256 |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ for the io_uring path; anything older (or io_uring disabled) uses epoll |

## Connections

The score is the number of users held, and every user is a keep-alive connection, so the
connection count matters more than anything else.

- Accepted sockets are io_uring **fixed (direct) descriptors**: they live in the ring's file
  table, not in the process fd table, so `LimitNOFILE=65535` doesn't cap them. A file table can't
  be larger than `RLIMIT_NOFILE` either, so under that limit the server makes 8 rings of 65,535
  slots (524,280 connections); with a 1M limit it makes one ring.
- One thread drives all of them. Only one ring accepts at a time, ring 0 first, so up to 65,535
  connections the loop is the plain single-ring loop: **one `io_uring_enter` per batch**, which
  submits the previous batch's sends and waits for the next completions (`DEFER_TASKRUN`,
  `SINGLE_ISSUER`). When ring 0 is full (`-ENFILE`) accepting moves to the next ring, every ring
  gets an eventfd, and the thread sleeps in `epoll_wait` on those.
- Each connection has one **multishot recv** into a provided-buffer ring (4,096 × 2 KB per ring),
  so an idle connection holds no buffer at all. Receives that run out of buffers are re-armed
  after the batch, once that batch's buffers are back, never in a loop inside one batch.
- Connection records are a `calloc`'d table indexed by connection id; pages nobody touched stay
  unmapped. A generation number in every completion's `user_data` drops stale completions after a
  slot is reused.
- Closing is `SHUTDOWN` hard-linked to a fixed-file `CLOSE`; the shutdown ends the multishot recv.

Locally, with the server limited to 65,535 descriptors, it held 80,000 connections from wrk
(160,000 sockets on the box) with 30 open fds in the process.

## A batch

Each wake-up of the loop is one batch:

1. Every completion is handled. Each request is parsed in place in the receive buffer (a
   partial one is copied out to a per-connection buffer and finished when the rest arrives) and
   answered straight into the batch's output arena.
2. All of the batch's SQL runs in **one SQLite transaction**, begun lazily by the first query:
   reads share it, and writes are group-committed (rule 6). Nothing is sent before `COMMIT`
   returns. If the commit fails, every response in the batch is replaced by a 500 and its
   connection closed, so no 201 ever claims a write that didn't land.
3. Each connection's responses go out as one `SEND` pointing into the arena (no copy). An arena
   stays reserved until its last send completes; the next batch takes a free one. One send per
   connection is in flight at a time; anything produced meanwhile is queued and sent after it.

## Where the time goes

- **No allocation on the request path, and no GC.** `-Dgc_none` makes `GC.malloc` plain `malloc`
  and nothing is ever collected: buffers are allocated at start-up or grown with `realloc`, and the
  request path doesn't create Crystal objects. RSS stays flat.
- **SQLite with PGO.** `build.sh` compiles the amalgamation instrumented, runs
  [`pgo/train.c`](pgo/train.c) (the server's own statements in the benchmark's proportions, on a
  synthetic database it generates: 5k users, 40k posts, 160k likes, none of the seed), then
  recompiles with the profile. `-DSQLITE_THREADSAFE=2`, memstat off and unused features compiled
  out. `bash build.sh --no-pgo` skips the profile.
- **The feed in three short index walks.** The newest 20 ids come off the `(created_at, id)`
  index alone. When they fall within 256 ids of each other (they do: ids and timestamps both
  grow), their rows are one rowid range scan of `posts` joined to `users`, and their like counts
  one `GROUP BY` range scan of `likes_post_id_idx`; posts in the range that aren't in the top 20
  are skipped and the 20 are put back in order. Otherwise it runs the plain query. Every row is
  read live inside the request's transaction (rule 5).
- **Checkpoints off the hot thread.** `wal_autocheckpoint` is off; a second connection on a plain
  pthread runs a `PASSIVE` checkpoint every 20 ms, so the event loop never waits on the fsync.
- **JWT.** HMAC-SHA256 with the key's inner and outer pad states hashed once at start-up, so a
  check hashes only the token's `header.payload` plus one block for the outer hash. Every
  signature is verified on every request (rule 5). Constant-time compare, `alg` must be `HS256`, `exp` and `sub` validated.
- **HTTP and JSON.** One `memmem` for the header end, only the headers the API uses are looked at.
  The JSON validator checks the whole body without allocating and records where the wanted
  fields are; escaped strings are unescaped once into a scratch buffer. Output strings are
  escaped 8 bytes at a time (SWAR), integers formatted two digits at a time, and
  `Content-Length` is back-filled into a fixed-width slot instead of building the body first.
- Prepared statements (`SQLITE_PREPARE_PERSISTENT`) for everything, `mmap_size` 1 GB, 64 MB page
  cache, `temp_store=MEMORY`.

## Measurements

Same machine for both (Ryzen 5 3600, Ubuntu 24.04 container on WSL2, the server pinned to one
core, wrk on ten others), compared with the current #1, `rust-epoll-scooter1337`, built from its
folder:

| Workload | this | rust-epoll-scooter1337 |
|---|---:|---:|
| `/feed`, 256 connections | 26,200 req/s | 27,300 req/s |
| mix (46% feed, 46% post, 7% like, 1% create), 256 connections | 37,500 req/s | 37,800 req/s |
| same mix, 12,000 connections, server at 65,535 fds | 36,800 req/s | 34,100 req/s |

So about even on few connections and ahead on many. These are local numbers, not a score.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`NO_URING=1` forces the epoll loop (level-triggered, one SQLite transaction per `epoll_wait`
batch, same arena and group commit). `URING_SLOTS=n` caps the slots per ring, to test the
spill-over to more rings with few connections.

## Files

| | |
|---|---|
| `src/main.cr` | reads the env, opens the DB, starts the server |
| `src/uring.cr` | rings, accept/recv/send/close, the batch loop |
| `src/net.cr` | connection records, arenas, request framing; the epoll fallback |
| `src/app.cr` | routes, validation, fixed responses |
| `src/db.cr` | statements, the batch transaction, feed/post/create/like, the checkpointer |
| `src/http.cr`, `src/json.cr`, `src/jwt.cr`, `src/buf.cr` | parsing and output |
| `src/sys.cr`, `src/sqlite.cr` | the C bindings it needs |
| `pgo/train.c` | SQLite PGO training run (build time only) |
