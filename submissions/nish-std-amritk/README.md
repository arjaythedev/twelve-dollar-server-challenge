# Nish / standard library

Front: **direct** (serves `HOST:PORT` itself, `0.0.0.0:80` on the benchmark box)

Nish (a statically typed TypeScript subset compiled to native code through
LLVM), built with clang-18 and lld-18 from LLVM 18. The compiler runs on
Node.js 22.18.0 at build time only; the server is one native binary. Every
import is Nish's own standard library: `nish:net` for sockets and io_uring
ring slots, `nish/net/prefork` for the processes, `nish/sqlite` (the SQLite
amalgamation the compiler vendors, read through its B-tree cursors and
written through prepared statements), `nish/json` and `nish/json-tokenizer`,
`nish/bytes` and `nish/crypto/hmac`. No third-party package is used, and
SQLite is unmodified.

**Builds with Nish 0.49.0**: this version reads rows with `Cursor.record`,
`RowRecord` and `Database.reopenCursor`, writes JSON strings with
`jsonPutString`, and relies on that release's io_uring runtime (below),
whose wide rings (`NET_LISTEN_RING_WIDE`) are new in 0.48.0. In 0.49.0 a
commit after a B-tree split no longer walks the whole page-cache hash table,
which was 1.5% of the server's CPU at 250,000 users with this entry's 64 MiB
cache.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- **One process on io_uring ring slots.** Serving direct, every k6 user holds
  a keep-alive connection, and `LimitNOFILE=65535` caps one process's
  descriptors. So the listener accepts into io_uring ring slots
  (`NET_LISTEN_RING` in `nish:net`) rather than descriptors: a connection is a
  slot in a ring's file table, and one serving process holds up to `SLOTS`
  (400,000) of them. `NET_LISTEN_RING_WIDE` makes each ring 65,536 slots
  rather than 16,384, so 300,000 connections sit in five rings, not
  nineteen: the loop pays one `io_uring_enter` a busy ring each pass, and
  while busy it sleeps in its first ring rather than in epoll. A supervisor starts that process and one checkpointer,
  and restarts neither. Nish's runtime also registers the rings' provided
  buffers the way Ubuntu's 6.8.0-136 and later kernels accept (Launchpad
  #2162843), so a kernel update on the box would not drop it to the fallback.
- **Fallback.** Before any worker starts, a probe child opens a ring listener
  on a loopback port. Where the kernel refuses rings, or with `RING=0`, the
  supervisor starts `WORKERS` (5) processes on descriptors instead, each
  holding at most `SLOTS` (60,000). Either way each serving process binds
  `HOST:PORT` with `SO_REUSEPORT` and `TCP_NODELAY` and runs one
  single-threaded loop with its own SQLite connection.
- **HTTP/1.1 of its own** (`src/http.ts`), over `nish:net`'s `netRead` and
  `netWrite`. A request's head is parsed where it was read, in one shared
  buffer, its lines found sixteen bytes at a time by `nish/bytes`'s
  `skipJsonPlain` kernel, which stops at CR and LF. A connection's whole state
  is one 32-byte row of one array, so a request touches one cache line of it;
  only a connection with a head or body not yet whole, or with output the
  socket did not take, holds a buffer, drawn from a pool and given back. A
  response's head is written into room kept in front of its body, and the
  two leave in one `netWrite`. Readiness is level-triggered and stays armed,
  so a request costs no re-arm. Pipelined requests are served in order; one
  behind a write waits for that write's commit. An idle connection is never
  closed.
- **Passes that gather.** After a pass of fewer than `BATCH_MIN` (64)
  events, with no write waiting for the lock, the loop waits `BATCH_MS`
  (1 ms) on an empty poll set before the next pass. Requests that arrive
  meanwhile are served together in the next pass: fewer waits and system
  calls a request, and one COMMIT for more writes.
- **One read transaction per pass.** A pass's first read opens a `BEGIN`
  and the pass ends it, so the pass's reads share one snapshot and one WAL
  read lock. Its four cursors are opened in it, into four `Cursor` objects
  made once (`Database.reopenCursor`): there is no garbage collector, and a
  new object a pass stayed allocated.
- **Posts through SQLite's B-tree cursors.** `GET /feed` takes the 20 newest
  ids from `posts_created_at_id_idx` from its first entry, which is exactly
  `(created_at DESC, id DESC)` order. Their like counts come first, as one
  walk of `likes_post_id_idx` when the ids span less than 256 (a seek each
  otherwise). Then each post is read in the feed's order and written straight
  into the answer: a post whose id is one below the last one's is the
  cursor's previous row, on the leaf the last read left, any other a seek.
  A row is copied out whole with `Cursor.record`, SQLite's record format as
  stored, and decoded by `nish/sqlite`'s `RowRecord`; its author is a seek in
  `users`.
  A post or author that is missing, which the reference query's JOIN would
  drop, hands the request to SPEC.md's reference query. Each cursor open
  checks the table's or index's columns against `sqlite_schema`, so a
  different schema is refused, not misread; then, or with `CURSORS=0`, the
  reads go through SQL. Every response is byte for byte the SQL path's.
- **Durable group commit.** A pass's writes go as one group after its reads:
  `BEGIN IMMEDIATE`, each `INSERT … RETURNING` or like, one `COMMIT` (WAL,
  `synchronous=NORMAL`), and each response is sent only after that `COMMIT`
  returns (rule 6). If the group fails it is rolled back and every write in it
  answers 500. `busy_timeout` is 0: a group that finds the write lock taken
  (the checkpointer's RESTART) stays queued, and every pass tries it again.
- **Likes as a lookup and a one-row insert.** The post is looked up first,
  inside the group's write transaction, then `INSERT … VALUES … ON CONFLICT DO
  NOTHING`. An `INSERT … SELECT` may write many rows, so SQLite gives it a
  statement journal, which inside a group copied each page the group had
  already changed.
- **Checkpoints off the request path.** The checkpointer runs a PASSIVE
  checkpoint every `CHECKPOINT_MS` (50); once the log holds `RESTART_FRAMES`
  (2,048) frames a RESTART checkpoint holds writers off briefly, copies the
  rest and syncs. SQLite syncs the database file only at a checkpoint that
  copied the whole log, so under steady writes the RESTART would write out
  every page the PASSIVE checkpoints dirtied, holding the write lock
  throughout. The checkpointer syncs the file itself first, without the lock,
  leaving the RESTART only its last frames. Its `busy_timeout` is 0: a
  RESTART that a reader blocks gives up the lock at once and is tried again a
  millisecond later, rather than held through SQLite's backoff. The serving
  processes' `wal_autocheckpoint` (16,384 pages) is only a backstop.
- **Rows straight into JSON.** Text goes from the record into the response
  through `nish/json`'s `jsonPutString`, whose runtime kernel finds the bytes
  to escape and copies the rest in blocks of 16, 8 or 4 bytes. The post
  object's keys are pre-encoded.
- **Clocks once a pass.** The Date header and every token's `exp` check read
  the pass's one wall-clock reading.
- **Tokens.** HS256 over an HMAC keyed once with `JWT_SECRET`; each signature
  copies the two keyed states. Signature, `alg` (HS256 only) and `exp`
  (required) are checked on every request, then `sub` (a positive integer
  string) and `username` (a string). base64url is decoded four characters at
  a time by table. Nothing is cached between requests.
- **Request bodies.** A full JSON tokenizer (invalid UTF-8 and trailing bytes
  are malformed; the last `body` wins, as in `JSON.parse`), trimmed with
  JavaScript's `trim()` whitespace set and counted in code points, the unit of
  the schema's `CHECK`.
- **A 64 MiB page cache** on rings (16 MiB a process on descriptors), which
  measured 2.4% less CPU a request than 16 MiB.

Built with `--allow-panics`: since 0.44.0 Nish refuses by default any index or
arithmetic it cannot prove in range, and with the flag those sites keep the
run-time check every earlier release compiled.

No caches of responses, rows or tokens; no schema, kernel or CPU tuning. The
process only raises its own soft descriptor limit to the hard one.

Optional settings, with their defaults: `RING` (1; 0 keeps descriptors),
`CURSORS` (1; 0 reads posts through SQL), `BATCH_MIN` (64), `BATCH_MS`
(1; 0 never waits), `WORKERS` (1 on rings, 5 on descriptors), `SLOTS`
(400000 on rings, 60000 on descriptors) per process, `CACHE_KIB` (65536 on
rings, 16384 on descriptors) SQLite page cache per process, `MMAP_MIB` (0),
`CHECKPOINT_MS` (50), `RESTART_FRAMES` (2048), `AUTOCHECKPOINT` (16384),
`CHECKPOINT_NICE` (0; the checkpointer's `nice`), `STATS` (0; 1 prints the
loop's and the checkpointer's counters every 10 s).

## Measured

Local figures, not official scores: an AMD Ryzen 9 9950X (Linux 7.3). The
server is pinned to one core with `taskset`, its SMT sibling left idle, and a
Go load generator on the other cores gives `bench/load.js`'s user model (a
feed, one of its posts, a like 15% of the time and a new post 2% of the
time, each user on its own keep-alive connection) with its think times
scaled. Loopback conntrack is off, as the box has none. CPU is the core's
busy time, and the server processes' own user and system time, divided by
the requests completed. The same harness ran
[rust-iouring-kensac](../rust-iouring-kensac) (310,000 on the leaderboard)
for reference.

300,000 connections at about 96,000 requests a second, a 5-minute hold:

| Server | Core per request | Process per request | p99 | Errors |
| --- | ---: | ---: | ---: | ---: |
| this entry, Nish 0.46.0 (the code before this version) | 10.5 µs (saturated) | 8.2 µs | 285 ms | 0 |
| this entry | 7.5–7.7 µs (72–74% of the core) | 5.2–5.4 µs | 2.5 ms | 0 |
| rust-iouring-kensac | 6.7 µs (64%) | 3.9 µs | 2.7 ms | 0 |

What took it there, measured A/B on this harness:

- Nish's io_uring runtime re-entered the ring while the kernel flagged
  pending task work, which under steady load meant about 11
  `io_uring_enter` calls a request at 150,000 connections. It now stops once
  an entry reaps fewer than 16 completions: 1.5 calls a request, and 15% less
  of the process's CPU a request there. At 300,000 connections and 96,000
  requests a second, where passes are fuller, the two measured the same.
- This server's own HTTP/1.1 in place of `nish/net/http1-server` cut cache
  misses a request by 14% and user cycles by 3–10%.
- Reading rows with `Cursor.record` and decoding them here, `jsonPutString`,
  the feed read in its own order through `previous()` and one clock read a
  pass each took 1–5% off user cycles or instructions.

All 42 checks of `test/test.sh` pass. The cursor path answers byte for byte
as the SQL path (`CURSORS=0`) does for the feed and 1,500 posts across the
seed, and 2,303 bodies with every escape JSON needs at every offset read
back as `JSON.stringify` writes them.

## Credits

Ideas taken from other submissions, each written here from the idea, not the
code:

- [c-epoll-unorig-username](../c-epoll-unorig-username): one SQLite
  transaction per loop pass; a checkpoint helper process that keeps
  checkpoints off the request path; and starting more processes on the same
  port past the descriptor limit (the five-process design `RING=0` keeps).
- [rust-iouring-kensac](../rust-iouring-kensac): reading posts through
  SQLite's own B-tree cursors rather than its bytecode engine, decoding the
  record format directly; holding every connection in one io_uring process;
  holding writes while a checkpoint catches up so the WAL can start again;
  the feed's like counts (and, through SQL, its rows) as one range walk of the
  index when the ids lie close together; one send per response; a 64 MiB page
  cache; and letting a loop pass gather for about a millisecond.
- [rust-iouring-kensac](../rust-iouring-kensac) and
  [c-iouring-pavlyhalim](../c-iouring-pavlyhalim): retrying a refused
  provided-buffer registration with a nonzero reserved word, for Ubuntu's 6.8
  kernels (in Nish's runtime).

[c-iouring-pavlyhalim](../c-iouring-pavlyhalim) and
[c-epoll-unorig-username](../c-epoll-unorig-username) were also built and run
under this entry's load harness, as reference points for its local figures;
no code of theirs is used. SPEC.md's reference SQL is the fallback for the
feed and the SQL path's query for a single post.
