# x86-64 assembly (NASM) + SQLite, epoll, direct

A feed API server written in x86-64 assembly. Every piece of application logic is hand-written
assembly: the HTTP/1.1 parser (keep-alive, pipelining, `Content-Length`, `Connection`, `Expect:
100-continue`, HTTP/1.0), routing, a strict RFC 8259 JSON validator with full string decoding
(`\uXXXX` and surrogate pairs), base64url, SHA-256 and HMAC-SHA256, the JWT checks, JSON output with
escaping, integer formatting, and the epoll event loop. Sockets, epoll, memory mapping and signals
are raw `syscall`s. The only external code is SQLite.

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 `nasm`) |
| Assembler / linker | `nasm -f elf64`, linked with `gcc` (libc provides `_start`/CRT so SQLite's C runtime initializes) |
| SQLite | 3.54.1, official amalgamation, SHA-256 pinned, compiled from source by `build.sh` |
| **Nginx or direct** | **Direct**: the server speaks HTTP/1.1 on `HOST:PORT` itself, no Nginx |
| Dependencies | one (SQLite). No HTTP library, JSON library, crypto library or framework. |

## Build and run

```bash
sudo bash install.sh     # apt: build-essential, nasm, curl, unzip
bash build.sh            # downloads + verifies SQLite, assembles, links bin/server
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`main` is the entry point (so the C runtime SQLite relies on is set up); it reads the four
environment variables itself from `envp`, and calls only `sqlite3_*` functions from libc.

## Design

It mirrors the companion C server (`submissions/c-epoll-unorig-username`): one level-triggered epoll
loop per process, SQLite opened in-process, and normally exactly one server process (plus the
checkpoint helper below). Config comes only from the four environment
variables in SPEC.md.

- **Group commit, one transaction per loop iteration.** Each `epoll_wait` wakeup opens a single
  deferred `BEGIN` lazily on first use, serves every ready request into a 4 MiB response arena, then
  `COMMIT`s once and only then writes the batched responses. A `201` is sent strictly after its
  commit; if the commit fails, every connection in that batch is dropped rather than told its write
  succeeded.
- **Feed: newest ids first, then two range scans.** `/feed` first reads the 20 newest ids from the
  `(created_at, id)` index. When they span fewer than 256 ids (new posts get consecutive ids, so this
  is the normal case), one scan of `likes_post_id_idx` over that range counts the likes and one scan
  of `posts` over it fetches the rows, which are then put back in feed order. That replaces 20
  separate count subqueries and 20 join lookups. If the ids are spread out, or a row doesn't turn up,
  it runs the reference query instead. Both run in the batch's read transaction, so they see one
  snapshot. Measured locally (amd64 under Rosetta, 1 CPU, wrk 64 connections, five alternating
  10-second runs each): feed-only 49,700 to 62,500 req/s (20.1 to 16.0 us CPU per request), the
  benchmark mix 59,400 to 68,800 req/s.
- **No caching across requests.** Every request reads SQLite while it is served. Prepared statements
  are reused (query plans only, not data). Every JWT signature is verified on every request with
  hand-written HMAC-SHA256; there is no verification cache.
- **Durability.** `journal_mode=WAL`, `synchronous=NORMAL`, `mmap_size=1 GiB`, `cache_size=16 MiB`,
  normal locking. No `synchronous=OFF`, no in-memory database.
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same binary,
  fork+exec'd as `server --ckpt` by the original process only) runs `sqlite3_wal_checkpoint_v2`
  `PASSIVE` in a loop, sleeping 1 ms while it is behind and 10 ms otherwise. The event loop never
  waits on the disk, and the WAL stays a few MB under normal load. If the helper can't keep up (the
  CPU is pegged for minutes), the server's own `wal_autocheckpoint=65536` still caps the WAL at about
  256 MB. Durability is unchanged: WAL with `synchronous=NORMAL`, and a response waits for its
  `COMMIT`.
- **Elastic workers.** `LimitNOFILE` is per process and every keep-alive client holds a descriptor.
  With `lim = min(RLIMIT_NOFILE, 65536)`, at `lim - max(lim/16, 64)` connections the process pauses its
  listener and fork+execs a copy of itself (`--shared <fd>`) that
  listens on the same port via `SO_REUSEPORT`. Once the child signals it is listening (one byte on a
  `pipe2(O_CLOEXEC)` pipe), the parent accepts what is already queued and closes its listener; it
  reopens it when it drops below 7/8 of the threshold. Children get `PR_SET_PDEATHSIG`, `SIGCHLD` is
  ignored, and all descriptors are close-on-exec. Workers don't start another checkpointer. With several processes, a write that hits
  `SQLITE_BUSY_SNAPSHOT` ends the read part of the batch, takes `BEGIN IMMEDIATE` and retries once.
  Below the threshold (every normal run) there is a single process.
- **Connections.** Non-blocking sockets, `TCP_NODELAY`, `SO_REUSEPORT`/`SO_REUSEADDR`. Partial reads
  and partial writes are buffered per connection (buffers come from `mmap`, not a general allocator).
  `RLIMIT_NOFILE` is raised to its hard limit at startup. Idle keep-alive connections are kept open
  (no idle timeout), so Nginx, if used, never reuses a socket the server just closed.
- **SQLite build flags.** `SQLITE_THREADSAFE=0` (single thread), plus the usual size/speed omits.

### Correctness notes

- JSON strings that do not fit their decode buffer (for example an unknown key longer than 32
  bytes) are still validated through their closing quote, so they never turn a valid document into a
  400.
- JSON parsing is strict (rejects trailing bytes, bad escapes, control characters, invalid UTF-8).
  Request-body string length is counted in Unicode code points for the 500-character limit.
- Body trimming removes the same whitespace set as JavaScript `String.trim` (ASCII plus
  U+00A0, U+1680, U+2000–U+200A, U+2028/2029, U+202F, U+205F, U+3000, U+FEFF).
- On authenticated routes, auth is checked before the id, matching the spec.
- Every call into SQLite is made with a 16-byte aligned stack, as the System V ABI requires
  (SQLite's compiled SSE code faults on a misaligned one on real x86-64 hardware).
- The stack is marked non-executable (`.note.GNU-stack`).

### Limitations

- `Transfer-Encoding` (chunked) request bodies are not supported (out of scope per SPEC.md); such a
  request closes the connection.

## License

MIT, under the repo's [license](../../LICENSE).
