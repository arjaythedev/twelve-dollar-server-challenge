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

One process, `GOMAXPROCS=1`, two OS threads, each with its own SQLite connection.

- **Loop thread.** Edge-triggered epoll owns the listener and every socket. It parses HTTP/1.1,
  verifies the bearer token and runs every query, reads and writes alike, on one connection with
  prepared statements. There is no goroutine per connection: an idle socket costs one ~100-byte
  struct, so 60,000 keep-alive connections are a few megabytes of heap and the GC has nothing to scan.
- **Group commit.** Writes that arrive in the same epoll batch share one transaction. Their 201s are
  held in the socket's write buffer until `COMMIT` returns, then flushed; a failed commit turns them
  into 500s. A lone write is a plain autocommit.
- **Checkpointer thread.** `wal_autocheckpoint` is off on the loop connection. The second connection
  runs `PRAGMA wal_checkpoint(PASSIVE)` every 2 s and `RESTART` once the WAL passes 32 MB, so the
  fsync of a checkpoint never stalls the loop. At 60k users the WAL would otherwise hit the default
  autocheckpoint about once a second.
- **Connection budget.** Above 60,000 open connections (or `RLIMIT_NOFILE − 128` if smaller),
  completed responses carry `Connection: close`; the client reconnects for its next request. This
  keeps the process under the 65,535-descriptor limit with room for the database, WAL and epoll.
- **Hand-written JSON** with `strconv` and a small escaper. Request bodies and JWT claims are
  parsed with `encoding/json`, which is the only reflection on the request path.
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON` (a like on a missing post
  is detected from the foreign-key error, one statement), `busy_timeout=5000`, `temp_store=MEMORY`,
  `mmap_size=256 MiB` (the 250 MB file is read from the page cache without copies), `cache_size=8 MiB`.
- Idle connections close after 75 s. Pipelining works. Chunked uploads get 411, bodies over 16 KB 413.

## Why

The benchmark's limit is connections, not request cost: at 64k users it is ~6.8k requests per second,
about 150 µs of CPU per request on one core, but 64k open sockets. `net/http` spends that budget on
goroutine stacks and GC. An event loop spends it on the request.

## License

MIT, under the repo's [license](../../LICENSE).
