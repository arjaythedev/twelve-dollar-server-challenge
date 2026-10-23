# Go + epoll + SQLite

| | |
|---|---|
| Language | Go 1.27.2 |
| Framework | none: own epoll event loop and HTTP/1.1 parser on `golang.org/x/sys/unix` v0.49.0 |
| SQLite | 3.54.0 amalgamation, compiled in by cgo through a ~150-line binding in `internal/sqlite` (no database/sql, no driver package) |
| JWT | `crypto/hmac` + `crypto/sha256`, verified on every request |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80`) itself |

## Running it

```bash
sudo bash install.sh   # build-essential, curl, unzip; Go 1.27.2 into /usr/local/go
bash build.sh          # downloads the pinned SQLite amalgamation (sha256-checked), cgo build to bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## How it works

One process, `GOMAXPROCS=2`, two OS threads, each with its own SQLite connection.

- **Loop thread.** Edge-triggered epoll owns the listener and every socket. It parses HTTP/1.1,
  verifies the bearer token and runs every query, reads and writes alike, on one connection with
  prepared statements. There is no goroutine per connection: an idle socket costs one ~100-byte
  struct, so 60,000 keep-alive connections are a few megabytes of heap and the GC has nothing to scan.
- **Group commit.** Writes that arrive in the same epoll batch share one `BEGIN IMMEDIATE … COMMIT`.
  Their 201s are held in the socket's write buffer until `COMMIT` returns, then flushed; a failed
  commit turns them into 500s.
- **Checkpointer thread.** `wal_autocheckpoint` is off on the loop connection, because at 60k users
  the WAL would hit the default autocheckpoint about once a second and each one fsyncs on the loop.
  Instead the loop asks a second thread, with its own connection, for a `PASSIVE` checkpoint every
  2 s; that copies the bulk of the frames and syncs without ever blocking a reader or writer. SQLite
  only restarts a WAL when a writer begins while every frame is backfilled and no reader is inside
  it, which never happens by itself while the loop keeps committing during each pass. So after a
  bulk pass, if the WAL file is at its 64 MiB limit, the loop parks incoming writes (their bytes
  stay in the socket buffer; reads are still served) and asks for one more pass: with no new frames
  arriving it copies only the handful committed during the bulk pass and syncs, in tens of
  milliseconds. The loop then releases the writes and the first one restarts the WAL at frame 0.
  The file is left to grow to 64 MiB once, early in the run; `journal_size_limit` then holds it at
  exactly that size, so from there on every 2 s pass ends with a restart. That cadence is what keeps
  reads cheap: a page written on every commit leaves one entry per commit in the wal-index hash
  table, and every lookup walks the chain, so a WAL that restarts every 2 s stays a few thousand
  frames deep instead of filling 4,096-frame tables. The loop itself never fsyncs. RESTART or
  TRUNCATE from the other thread would instead hold the write lock while waiting on readers and
  stall every request for the whole busy timeout.
- **Connection budget.** Above 60,000 open connections (or `RLIMIT_NOFILE − 128` if smaller),
  completed responses carry `Connection: close`; the client reconnects for its next request. This
  keeps the process under the 65,535-descriptor limit with room for the database, WAL and epoll.
- **Hand-written JSON** with `strconv` and a small escaper. Request bodies and JWT claims are
  parsed with `encoding/json`, which is the only reflection on the request path.
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON` (a like on a missing post
  is detected from the foreign-key error, one statement), `busy_timeout=5000`, `temp_store=MEMORY`,
  `mmap_size=256 MiB` (the 250 MB file is read from the page cache without copies), `cache_size=8 MiB`.
- **`SQLITE_ENABLE_QPSG`** at compile time. Without it SQLite 3.54 re-plans a statement on the first
  step after a parameter compared with a column is rebound, so `GET /posts/:id` would recompile its
  query from SQL on every request.
- **One read per request.** The loop stops reading a socket after a short read instead of reading
  again for the EAGAIN: a short read means the receive queue was empty at that moment, and a packet
  arriving later raises a new edge. That is one syscall fewer for nearly every request. A FIN that
  lands in the same edge as the request is the exception: once EPOLLRDHUP has been seen the loop
  reads on until the read that returns 0, so a half-closing client still gets its response and a
  prompt close.
- **The scheduler never sees the loop block.** epoll_wait goes through a raw syscall. Through the
  ordinary wrapper, Go's sysmon retakes the P from any goroutine blocked longer than 20 µs and the
  loop then buys it back with a futex hand-off on every return, several thousand extra syscalls a
  second under load, for nothing, since nobody else wants that P: the second P is where the
  checkpointer, the garbage collector and signal delivery run, so none of them waits on sysmon to
  notice a blocked thread (with a single P the checkpointer's own cgo calls stalled the loop by up
  to sysmon's 10 ms poll, and p99 showed it). The wait is still capped at 10 ms and the loop yields
  when it expires, so the design also holds with one P. An EINTR returns to the loop instead of
  retrying, because that signal is usually the scheduler asking for a preemption point; retrying in
  place would spin in the syscall while a stop-the-world waits on this P.
- Idle connections close after 75 s. Pipelining works, capped at 32 requests per connection per loop
  round so one flooding socket cannot starve the rest. After a `Connection: close` response the
  server half-closes, discards further input and waits for the peer's FIN rather than sending an RST.
  Chunked uploads get 411, bodies over 16 KB 413.

## Why

The benchmark's limit is connections, not request cost: at 64k users it is ~6.8k requests per second,
about 150 µs of CPU per request on one core, but 64k open sockets. `net/http` spends that budget on
goroutine stacks and GC. An event loop spends it on the request.

## License

MIT, under the repo's [license](../../LICENSE).
