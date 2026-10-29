# Go + raw epoll + SQLite (cgo)

| | |
|---|---|
| Language | Go 1.27.2 (official tarball, SHA-256 pinned in `install.sh`), built with `GOAMD64=v2`, `-trimpath` |
| Framework | none: a raw `epoll` loop through package `syscall`, hand-written HTTP/1.1, JSON and JWT parsing |
| SQLite | 3.54.1, the official amalgamation (SHA-256 pinned in `build.sh`), compiled into the binary through cgo |
| Other dependencies | none: only the standard library (`crypto/hmac`, `crypto/sha256`, `encoding/base64`, `syscall`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip; Go 1.27.2 into /usr/local/go
bash build.sh          # downloads and verifies SQLite, builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## What is not Go

`main.go` has a 53-line cgo preamble: `#include "sqlite3.h"` plus small helpers that bind, step and
read a whole row's columns in one call (and run an insert with `RETURNING` to completion in one call).
Each cgo crossing costs far more than a C function call, so this keeps a feed request at about 22
crossings instead of about 120. Everything else (HTTP, routing, JSON parsing and output, JWT checks,
transactions and batching, the event loop) is Go. `sqlite_amalg.c` is a one-line file that compiles the
downloaded amalgamation into the package.

## Design

- **One goroutine on one locked OS thread, `GOMAXPROCS=1`, one `epoll` loop.** The box has one vCPU.
  Go's `net` package and its goroutine-per-connection model are not used: sockets are raw non-blocking
  descriptors read and written with raw syscalls, and only `epoll_wait` goes through the scheduler (so
  the garbage collector can still stop the world while the loop sleeps).
- **No allocation on the hot path.** Responses are appended into one preallocated arena, requests are
  parsed in place, JWT parts are decoded into fixed buffers, and the HMAC (`crypto/hmac`, keyed once at
  startup) is `Reset()` per token. Under load the collector runs no cycles after startup. `GOGC=400`
  and a 1 GiB memory limit are set in code.
- **A transaction per loop iteration (group commit).** The first request in an `epoll_wait` batch that
  touches the database opens a deferred `BEGIN`; every read and write in the batch runs inside it and
  the batch ends with one `COMMIT`. Responses are only sent after that `COMMIT` returns, so a `201`
  always means the row is committed (rule 6). If a commit fails, nothing from that batch is sent: those
  connections are closed instead.
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same binary, started
  with `--ckpt` through `syscall.ForkExec`) runs `PASSIVE` checkpoints in a loop. The event loop never
  waits on the disk, and the WAL stays a few MB under normal load. If the helper can't keep up (the CPU
  is pegged for minutes), the server's own `wal_autocheckpoint=65536` still caps the WAL at about
  256 MB. Durability is unchanged: WAL with `synchronous=NORMAL`, and a response waits for its `COMMIT`.
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, persistent
  prepared statements, single-threaded build with unused features left out. The reference queries; a
  like is one `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`, with a post lookup
  only when nothing was inserted.
- **No caching** (rule 5): every request runs its query and every token's signature is computed.
- **Past 65,535 clients.** Serving direct, every k6 user holds a keep-alive connection, and
  `LimitNOFILE=65535` caps descriptors per process. Near that cap the process starts a copy of itself
  on the same port (`SO_REUSEPORT`, `syscall.ForkExec`, parent-death signal set), and stops accepting
  once the copy reports it is listening; it accepts again when it has room. Below the cap there is exactly one process. Workers share the database through WAL, and a write
  whose read snapshot was outdated by another worker's commit retries under `BEGIN IMMEDIATE`.
- **Strict input handling**: RFC 8259 JSON validation (last duplicate key wins), `\u` escapes and
  surrogate pairs, invalid UTF-8 rejected, JavaScript `String.prototype.trim` whitespace, the
  500-character limit in code points. JWT: HS256 only, constant-time compare, `exp`/`nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0 and
  `Expect: 100-continue`. Chunked request bodies aren't supported (out of scope).

## License

MIT, under the repo's [license](../../LICENSE).
