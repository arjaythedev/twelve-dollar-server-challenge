# Zig + raw epoll + SQLite

| | |
|---|---|
| Language | Zig 0.15.2 (official x86_64 Linux tarball, SHA-256 pinned in `install.sh`) |
| Framework | none: a hand-written HTTP/1.1 server on `std.os.linux` epoll syscalls |
| SQLite | 3.54.1 official amalgamation (SHA-256 pinned in `build.sh`), compiled into the binary by Zig's C compiler |
| Crypto | HMAC-SHA256 in `src/main.zig` (a plain SHA-256 loop, checked against `std.crypto` by a `zig test` block) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80` on the benchmark box) itself |

No third-party Zig packages. The only dependency is SQLite, built from source with the app.

## Running it

```bash
sudo bash install.sh   # Zig 0.15.2 into /opt, plus curl/unzip/xz
bash build.sh          # fetches + verifies SQLite, builds bin/server (ReleaseFast, x86-64-v2)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design and optimizations, and why

- **One process, one thread, level-triggered epoll.** The box has one vCPU, so threads would only
  add context switches and locking. Every query is an index lookup that takes microseconds.
- **Elastic workers past the descriptor limit.** Every keep-alive client holds a descriptor and
  `LimitNOFILE` (65,535) applies per process. When this process gets within `max(limit/16, 64)`
  descriptors of the limit, it stops accepting and re-executes itself (`/proc/self/exe --shared`) as a second worker listening on the same
  port with `SO_REUSEPORT`. Once the new worker reports that it is listening, the old one takes
  whatever is still queued and closes its listener, so new clients land on the new worker. It
  reopens its listener when it drops back below 7/8 of that mark. Workers die with their parent
  (`PR_SET_PDEATHSIG`). Below the limit there is exactly one process. With several workers a write that hits `SQLITE_BUSY_SNAPSHOT` ends the read-only part of the batch, takes
  `BEGIN IMMEDIATE` and retries, and a batch is never acknowledged unless its `COMMIT` succeeded.
- **Group commit per loop iteration.** All requests that arrive in one `epoll_wait` batch share one
  deferred SQLite transaction, opened lazily by the first request that touches the database. At
  the end of the batch the server runs `COMMIT`, and only then writes every response produced in
  that batch. A 201 is never sent before its row is committed, and reads stay live (each batch
  starts a fresh snapshot, so a like is visible to the next request). If a commit fails, the
  connections that got responses from that batch are closed instead of being told it worked.
- **Responses are built in one arena** (4 MiB, reset per batch) and sent with a single `write` per
  connection. Only a short write allocates a per-connection buffer and switches to `EPOLLOUT`;
  only a request split across reads allocates an input buffer. Idle connections hold no buffers,
  so tens of thousands of keep-alive clients cost almost nothing beyond the kernel socket.
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same binary, started
  with `--ckpt`) runs `PASSIVE` checkpoints in a loop. The event loop never waits on the disk, and the
  WAL stays a few MB under normal load. If the helper can't keep up (the CPU is pegged for minutes),
  the server's own `wal_autocheckpoint=65536` still caps the WAL at about 256 MB. Durability is
  unchanged: WAL with `synchronous=NORMAL`, and a response waits for its `COMMIT`.
- **Feed: newest ids first, then two range scans.** `/feed` first reads the 20 newest ids from the
  `(created_at, id)` index. When they span fewer than 256 ids (new posts get consecutive ids, so this is
  the normal case), one scan of `likes_post_id_idx` over that range counts the likes and one scan of
  `posts` over it fetches the rows, which are then put back in feed order. That replaces 20 separate
  count subqueries and 20 join lookups. If the ids are spread out, or a row doesn't turn up, it runs the
  reference query instead. Both run in the batch's read transaction, so they see one snapshot.
- **SQLite pragmas:** WAL, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size` (the whole 250 MB
  database is read through the page cache without copies), 16 MB page cache.
- **SQLite compile options:** `THREADSAFE=0` (single thread), `DEFAULT_MEMSTATUS=0`,
  `OMIT_SHARED_CACHE`, `OMIT_PROGRESS_CALLBACK`, `OMIT_DEPRECATED`, `USE_ALLOCA`, and friends: less
  work per statement step.
- **Prepared statements** are created once with `SQLITE_PREPARE_PERSISTENT` and reused (they cache
  the plan, not data). The SQL is the reference SQL from SPEC.md (plus the feed range scans above); a like is one
  `INSERT ... SELECT ... WHERE EXISTS(post) ON CONFLICT DO NOTHING`, with a post lookup only when
  nothing was inserted (to tell "already liked" from "no such post").
- **JWT verified on every request** (rule 5): HS256 only, constant-time signature compare, `exp`
  and `nbf` checked (`exp` is required). The HMAC key schedule (the hashed `key^ipad` / `key^opad` blocks) is computed
  once from `JWT_SECRET`, which saves two SHA-256 blocks per request without remembering any token.
  SHA-256 is our own plain loop: without CPU SHA instructions (`x86-64-v2` has none) the standard
  library's measured 4 to 7 times slower, a quarter of the CPU on writes. `zig test -lc
  -Ideps/sqlite-amalgamation-3540100 src/main.zig` checks it against `std.crypto`.
- **Strict JSON** for request bodies and JWT segments: a full RFC 8259 validator, `\u` escapes and
  surrogate pairs decoded to UTF-8, the last duplicate key wins (like `JSON.parse`). The post body
  is trimmed with the JavaScript `String.prototype.trim` whitespace set and its length is counted
  in Unicode code points, matching SQLite's `length()` in the schema's CHECK.
- **No idle timeout**: keep-alive connections stay open until the client closes them. The process
  raises its soft `RLIMIT_NOFILE` to the hard limit (65,535 on the benchmark box) and hands new
  clients to another worker before it runs out (see above).
- **Built with `-O ReleaseFast -mcpu x86_64_v2`**, not `native`, so it runs on any current droplet CPU.

Pipelined requests, requests split across reads and `Expect: 100-continue` are handled.
Chunked request bodies are not supported (the connection is closed); clients in this challenge
always send `Content-Length`.

## License

MIT, under the repo's [license](../../LICENSE).
