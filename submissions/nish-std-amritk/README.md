# Nish / standard library

Front: **direct** (serves `HOST:PORT` itself, `0.0.0.0:80` on the benchmark box)

Nish 0.44.0 (a statically typed TypeScript subset compiled to native code
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
  server memory. A response is its head and body in one `writev`.
- **One read transaction per pass.** A pass is everything one `epoll_wait`
  hands back. Its first read opens a `BEGIN`, and the pass ends it, so the
  pass's reads share one snapshot and one WAL read lock instead of taking and
  dropping it per statement. Every request still runs its own statements
  while it is served; nothing outlives the pass (rule 5).
- **Feed as range reads.** `GET /feed` reads the 20 newest ids from
  `posts_created_at_id_idx` alone. When they lie within 256 of each other, as
  the newest posts nearly always do, their rows and authors are one range
  scan of `posts` in id order (`WHERE p.id BETWEEN ? AND ?`, joined to
  `users`), and their like counts one range scan of `likes_post_id_idx`
  (`GROUP BY post_id`), instead of 20 seeks of each. Rows in the range that
  are not among the 20 are skipped. Otherwise, or if any of the 20 is missing
  from the range (a post without its user, which the reference query's JOIN
  would drop), SPEC.md's reference query answers. Measured alone, this took a
  feed from 47.9 to 39.7 µs.
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

Built with `--allow-panics`: Nish 0.44.0 refuses by default any index or
arithmetic it cannot prove in range, and with the flag those sites keep the
run-time check every earlier release compiled.

No caches of responses, rows or tokens; no schema, kernel or CPU tuning. The
process only raises its own soft descriptor limit to the hard one.

Optional settings, with their defaults: `RING` (1; 0 keeps descriptors),
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

Nish 0.44.0, three interleaved rounds of each 90,000-connection design (100 s
each, the first 20 s not counted), median with the range:

| Load | Design | req/s | CPU / req | p99 | server PSS |
| --- | --- | ---: | ---: | ---: | ---: |
| 256 connections, no think time | 1 process on rings | 43,000 | 22.8 µs (22.5–23.8) | 63–81 ms | 14 MB |
| 90,000 connections, 4 s per request each | 1 process on rings | 21,200 | 37.0 µs (35.5–41.7) | 118–220 ms | 45 MB |
| 90,000 connections, 4 s per request each | 5 processes on descriptors (`RING=0`) | 21,100 | 42.2 µs (40.6–43.1) | 45–124 ms | 17 MB |

Rings cost about 12% less CPU a request at 90,000 connections, one process
doing the work of five. Their tail is longer: with one process, a request
waits behind the burst ahead of it on the one core, where five processes
interleave. Both stay well inside k6's thresholds (p95 < 500 ms,
p99 < 1 s) at this load. PSS is the server's processes alone; the kernel holds
about 3.9 KB more a socket. With Nish 0.41.0 on descriptors, per route at
saturation on one worker: `GET /feed` 39.7 µs, `GET /posts/:id` 8.5 µs, a 404
that touches no database 5.1 µs; a new like 28 µs batched (59 µs alone), a
new post 44 µs (84 µs alone).

A profile-guided build (`nish --pgo-generate`, trained on this mix, then
`--pgo-use`) measured 23.1 µs against 22.7 µs at saturation over three rounds
each, no gain, so the build does not use it.

All 42 checks of `test/test.sh` pass.

## Credits

Ideas taken from other submissions, each written here from the idea, not the
code: a transaction per loop pass and a checkpoint helper process, and
starting more processes on the same port past the descriptor limit, from
[c-epoll-unorig-username](../c-epoll-unorig-username); holding writes while a
checkpoint catches up so the WAL can start again, from
[rust-iouring-kensac](../rust-iouring-kensac). SPEC.md's reference SQL is the
fallback for the feed and the query for a single post.
