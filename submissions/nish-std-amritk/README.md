# Nish / standard library

Front: **direct** (serves `HOST:PORT` itself, `0.0.0.0:80` on the benchmark box)

Nish 0.45.1 (a statically typed TypeScript subset compiled to native code
through LLVM), built with clang-18 and lld-18 from LLVM 18. The compiler runs
on Node.js 22.18.0 at build time only; the server is one native binary. Every
import is Nish's own standard library: `nish/net/http1-server` for HTTP/1.1,
`nish/net/prefork` for the processes, `nish/sqlite` (the SQLite amalgamation
the compiler vendors, called through its prepared-statement API),
`nish/json-tokenizer` and `nish/crypto/hmac`. No third-party package is used,
and SQLite is unmodified.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- **Processes and io_uring.** Serving direct, every k6 user holds a
  keep-alive connection, and `LimitNOFILE=65535` caps one process's
  descriptors. So the listener accepts into io_uring ring slots
  (`NET_LISTEN_RING` in `nish:net`) rather than descriptors: a connection is a
  slot in a ring's file table, and one serving process holds up to `SLOTS`
  (400,000) of them in a few dozen descriptors. Its receives are multishot and
  its sends are queued and submitted together. A supervisor starts that
  process and one checkpointer, and restarts neither.
- **Fallback.** Before any worker starts, a probe child opens a ring listener
  on a loopback port. Where the kernel refuses rings (Docker's default seccomp
  profile, a kernel without io_uring), or with `RING=0`, the supervisor starts
  `WORKERS` (5) processes on descriptors instead, each holding at most `SLOTS`
  (60,000), five holding 300,000. Either way each serving process binds
  `HOST:PORT` with `SO_REUSEPORT` and `TCP_NODELAY` and runs one
  single-threaded loop with its own SQLite connection; nothing else is shared.
- **HTTP/1.1** through `nish/net/http1-server`'s sans-IO connections. An idle
  keep-alive connection holds no input buffer (`idleInput`), about 1.2 KiB of
  server memory. A response's body is copied into the connection's output
  behind its head (`write`, not `sendBody`), and the two leave in one send:
  on a ring slot `sendBody`'s head-and-body write is two SENDs and, with
  `TCP_NODELAY`, two segments. This halved the TCP segments a request
  (4.0 to 2.0, both directions on loopback).
- **Passes that gather.** After a pass of fewer than `BATCH_MIN` (64)
  events, with no write waiting for the lock, the loop waits `BATCH_MS`
  (1 ms) on an empty poll set before the next pass, once the pass's responses
  are sent. Requests that arrive meanwhile are served together in the next
  pass: fewer waits, wakes and system calls a request, and one COMMIT for
  more writes. It adds at most a millisecond of latency.
- **One read transaction per pass.** A pass is everything one `epoll_wait`
  hands back. Its first read opens a `BEGIN`, and the pass ends it, so the
  pass's reads share one snapshot and one WAL read lock instead of taking and
  dropping it per statement. Every request still runs its own statements
  while it is served; nothing outlives the pass (rule 5).
- **Posts through SQLite's B-tree cursors.** `GET /feed` and `GET /posts/:id`
  read through `nish/sqlite`'s cursors (`Database.openCursor`), SQLite's own
  B-tree functions on the unmodified amalgamation, without its bytecode
  engine. The feed takes the 20 newest ids from `posts_created_at_id_idx`
  from its first entry, which is exactly `(created_at DESC, id DESC)` order.
  It then reads their posts and authors by rowid in ascending id order, so
  consecutive ids land on the next cell of the leaf the last seek left
  (SQLite's own fast path in `TableMoveto`) rather than a descent each. Their
  like counts are one walk of `likes_post_id_idx` when the ids span less than
  256, a seek each otherwise. A post or author that is missing, which the
  reference query's JOIN would drop, hands the request to SPEC.md's reference
  query. A single post is a post seek, an author seek and a like count. The
  four cursors are opened by the pass's first read, inside its read
  transaction, and closed when it ends, before the pass's writes commit. Each
  open checks the table's or index's columns against `sqlite_schema`, so a
  different schema is refused, not misread; then, or with `CURSORS=0`, the
  reads go through SQL. Every response is byte for byte the SQL path's.
- **Feed as range reads, through SQL.** Without cursors, `GET /feed` reads
  the 20 newest ids from `posts_created_at_id_idx` alone. When they lie within
  256 of each other, as the newest posts nearly always do, their rows and
  authors are one range scan of `posts` (`WHERE p.id BETWEEN ? AND ?`, joined
  to `users`), and their like counts one range scan of `likes_post_id_idx`
  (`GROUP BY post_id`), instead of 20 seeks of each. Otherwise SPEC.md's
  reference query answers.
- **Durable group commit.** A pass's writes go as one group after its reads:
  `BEGIN IMMEDIATE`, each `INSERT … RETURNING` or like, one `COMMIT` (WAL,
  `synchronous=NORMAL`), and each response is sent only after that `COMMIT`
  returns (rule 6). If the group fails it is rolled back and every write in it
  answers 500. `busy_timeout` is 0: a group that finds the write lock taken
  (another process's group, or the checkpointer's RESTART) stays queued and
  the next pass, at most a millisecond later, tries again, so the loop never
  sleeps in SQLite and reads go on being answered.
- **Likes in one statement.** `INSERT INTO likes … SELECT ?, ? WHERE EXISTS
  (post) ON CONFLICT DO NOTHING`; only when nothing was inserted does a lookup
  tell a repeat (200) from a missing post (404).
- **Checkpoints off the request path.** The checkpointer runs a PASSIVE
  checkpoint every `CHECKPOINT_MS` (50). Under steady writes that never copies
  everything, so the log would never start again from its beginning; once it
  holds `RESTART_FRAMES` (2,048) frames a RESTART checkpoint holds writers off
  briefly, copies the rest and syncs. The serving processes'
  `wal_autocheckpoint` (16,384 pages) is only a backstop. Without this the
  backstop ran inline and set p99 at about 115 ms; with it, 11 ms.
- **Rows straight into JSON.** Text columns are copied from SQLite into one
  reused buffer (`columnInto`) and written into the response with escapes
  only where JSON needs them, found 16 bytes at a time. The post object's
  keys are pre-encoded.
- **Tokens.** HS256 over an HMAC keyed once with `JWT_SECRET`; each signature
  copies the two keyed states. Signature, `alg` (HS256 only) and `exp`
  (required) are checked on every request, then `sub` (a positive integer
  string) and `username` (a string). Nothing is cached between requests.
- **Request bodies.** A full JSON tokenizer (invalid UTF-8 and trailing bytes
  are malformed; the last `body` wins, as in `JSON.parse`), trimmed with
  JavaScript's `trim()` whitespace set and counted in code points, the unit of
  the schema's `CHECK`.
- **No allocation per request.** Once a process has served its peak, a
  request allocates nothing: the server's RSS stays flat over millions of
  requests.

Built with `--allow-panics`: since 0.44.0 Nish refuses by default any index or
arithmetic it cannot prove in range, and with the flag those sites keep the
run-time check every earlier release compiled.

No caches of responses, rows or tokens; no schema, kernel or CPU tuning. The
process only raises its own soft descriptor limit to the hard one.

Optional settings, with their defaults: `RING` (1; 0 keeps descriptors),
`CURSORS` (1; 0 reads posts through SQL), `BATCH_MIN` (64), `BATCH_MS`
(1; 0 never waits),
`WORKERS` (1 on rings, 5 on descriptors), `SLOTS` (400000 on rings, 60000 on
descriptors) per process, `CACHE_KIB` (16384) SQLite page cache per process, `MMAP_MIB` (0),
`IDLE_TIMEOUT_MS` (600000), `CHECKPOINT_MS` (50), `RESTART_FRAMES` (2048),
`AUTOCHECKPOINT` (16384).

## Measured

Local figures, not official scores: a 4-core container (Intel Xeon at
2.10 GHz). The server is pinned to one core with `taskset`, and the load
generators (wrk, with a Lua script giving `bench/load.js`'s mix: a feed, one of
the newest posts, a like 15% of the time and a new post 2% of the time) run on
the other three. CPU is the user and system time of every server process from
`/proc`, divided by the requests completed. The container caps a process at
20,000 descriptors, so a test with many connections uses five wrk processes on
five loopback addresses.

The wrk script likes and opens posts from the last feed it read, as a k6 user
does. Nish 0.45.1, on one container, with two scored entries run under the
same harness for reference:

| Server | 90,000 connections, 4 s think | 180,000 connections, 8 s think | Leaderboard |
| --- | ---: | ---: | ---: |
| [c-iouring-pavlyhalim](../c-iouring-pavlyhalim) | 18.0 µs | 24.3 µs | 295,000 |
| this entry | 19.2 µs (18.9–19.7, 3 runs) | 24.9–25.0 µs (2 runs) | |
| [c-epoll-unorig-username](../c-epoll-unorig-username) | 30.2 µs | 43.4 µs | 210,000 |

This entry held every connection with no errors, at a p99 of 15 ms at 90,000
and 39–42 ms at 180,000, in 56 MB of server memory at 90,000 and 82 MB at
180,000. Saturated with 256 connections and no think time, it served about
70,000 requests a second at 13.5 µs each.

What each change took off, at 90,000 connections, A/B in the same session:

| Change | Before | After |
| --- | ---: | ---: |
| One send per response | 23.6 µs | 22.1 µs |
| Passes that gather (`BATCH_MS=0` against 1) | 22.0 µs (2 runs) | 19.4–21.0 µs (2 runs) |
| Feed in ascending id order, like counts in one walk | 20.3–21.5 µs (2 runs) | 18.9–19.7 µs (2 runs) |

Rings cost about 12% less CPU a request than five processes on descriptors
at 90,000 connections (an earlier session: 30.6 against 34.9 µs). A 2 ms
gather (19.9 µs) and a 64 MB page cache (21.0 µs) measured no better than
the defaults. A profile-guided build (`nish --pgo-generate`, trained on this
mix, then `--pgo-use`) measured 23.1 µs against 22.7 µs at saturation, no
gain, so the build does not use it.

**To check on the benchmark box.** With one serving process, a request waits
behind every request ahead of it in the pass on the one core, where five
processes interleave; the gather adds up to a millisecond more. At the loads
above the p99 stayed under 45 ms, but a post with many likes costs a cursor
step per like, so a run that piles likes on a few posts lengthens the tail.
If the box shows p95 or p99 trouble before it shows full CPU, `RING=0` is the
five-process design, `CURSORS=0` the SQL reads and `BATCH_MS=0` turns the
gather off.

All 42 checks of `test/test.sh` pass.

## Credits

Ideas taken from other submissions, each written here from the idea, not the
code: a transaction per loop pass and a checkpoint helper process, and
starting more processes on the same port past the descriptor limit, from
[c-epoll-unorig-username](../c-epoll-unorig-username); holding writes while a
checkpoint catches up so the WAL can start again, reading the feed's posts
in ascending id order with its like counts in one index walk, and letting a
pass gather for about a millisecond, from
[rust-iouring-kensac](../rust-iouring-kensac). SPEC.md's reference SQL is the
fallback for the feed and the query for a single post.
