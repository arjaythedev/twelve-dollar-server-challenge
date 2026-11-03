# COBOL (GnuCOBOL) + epoll + SQLite

| | |
|---|---|
| Language | COBOL, free format, one program (`server.cob`) |
| Compiler | GnuCOBOL 3.2, built from the official GNU tarball (SHA-256 pinned) by `install.sh` |
| Framework | none: the HTTP server, router, JSON and JWT code are all COBOL |
| SQLite | 3.54.1, official amalgamation (SHA-256 pinned), compiled with gcc in `build.sh` and linked in |
| Crypto | libcrypto's SHA-256 (`SHA256_Init/Update/Final`, Ubuntu 24.04 `libssl-dev`); HMAC itself is built in COBOL |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the box) |

## Running it

```bash
sudo bash install.sh   # apt toolchain + GnuCOBOL 3.2 from source (about 1-2 minutes)
bash build.sh          # fetches SQLite, builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## What is COBOL and what isn't

Everything the application does is COBOL: the epoll event loop and connection table, HTTP/1.1
parsing (keep-alive, pipelining, partial requests, `Content-Length`, `Connection: close`,
`Expect: 100-continue`), routing, a strict RFC 8259 JSON validator with `\uXXXX` and
surrogate-pair decoding, base64url decoding, JWT checks, HMAC-SHA256 assembly, JSON output
escaping and the group-commit batching.

There is no C glue file. COBOL calls these C functions directly with `CALL STATIC`:
libc system call wrappers (`socket`, `setsockopt`, `bind`, `listen`, `accept4`, `epoll_*`,
`read`, `write`, `close`, `signal`, `getrlimit`/`setrlimit`, `time`, `getenv`, `pipe2`, `fork`,
`execv`, `prctl`, `fcntl`, `usleep`), a few libc memory helpers (`malloc`, `free`, `memcpy`, `memmove`,
`strlen`, `atoi`, `inet_pton`), the SQLite C API, and libcrypto's SHA-256. Bit rotation is not
something COBOL does well, so the SHA-256 compression function comes from libcrypto; the HMAC
around it is COBOL: the `key^ipad`/`key^opad` blocks (via `CBL_XOR`) are absorbed once at startup
and the two hash states are kept, and every token's signature is then computed from copies of
those states (inner hash over `header.payload`, outer hash over the inner digest). `build.sh`
passes the real C headers to the C compiler (`-include ...`) so the static calls get correct
prototypes.

GnuCOBOL keeps a static call's return value in a C `int`, so no C function returns a 64-bit
value to COBOL here: integer columns (`id`, `like_count`, the new post's id) are read with
`sqlite3_column_text`, which hands back SQLite's decimal text (that also skips number
formatting), `time()` writes through its pointer argument, and ids are bound as 64-bit
`BY VALUE` arguments. Ids above 2^31 work.

## Design

- **One process, one epoll loop, SQLite called inline.** The box has one vCPU. Level-triggered
  epoll; each socket waits for either read or write readiness, never both.
- **Elastic workers for the descriptor limit.** Every keep-alive client holds a descriptor and
  `LimitNOFILE` (65,535) applies per process. When a process gets near its limit
  (`limit - max(limit/16, 64)` connections) it stops accepting and starts one more copy of itself (`fork` + `execv("/proc/self/exe",
  "--shared")`, with `PR_SET_PDEATHSIG`) that listens on the same port through `SO_REUSEPORT`.
  Once the copy reports over a pipe that it is listening, the parent accepts what is already
  queued and closes its listener; it opens one again when it drops below 7/8 of the threshold.
  With several processes, a write whose read snapshot went stale (`SQLITE_BUSY_SNAPSHOT`) ends
  the read-only part of the batch, takes `BEGIN IMMEDIATE` and retries. Below the limit there is
  one serving process.
- **Group commit.** The first request in an `epoll_wait` batch that touches the database opens a
  deferred `BEGIN`; reads and writes of that batch share it, and one `COMMIT` ends it. Every
  response of the batch waits in an arena until the commit succeeds, so a 201 is only sent for a
  committed row. If a commit fails, those connections are closed instead of answered.
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same binary, started
  with `--ckpt` via `fork` + `execv`, with `PR_SET_PDEATHSIG`) calls
  `sqlite3_wal_checkpoint_v2(PASSIVE)` in a loop. The event loop never waits on the disk, and the
  WAL stays a few MB under normal load. If the helper can't keep up (the CPU is pegged for minutes),
  the server's own `wal_autocheckpoint=65536` still caps the WAL at about 256 MB. Durability is
  unchanged: WAL with `synchronous=NORMAL`, and a response waits for its `COMMIT`.
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), normal (shared) locking,
  1 GiB `mmap_size`, 16 MB page cache. Prepared statements are reused; every request queries
  SQLite while it is served.
- **SQL**: the reference post query (`count(*)` subquery per post). A like is one statement,
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`; an existence check runs only
  when nothing was inserted, to tell "already liked" from "no such post".
- **Small per-connection state** (36 bytes per descriptor slot). Input is read into one shared 64 KB buffer; a
  connection only gets its own buffer while it holds a partial request, and an output buffer
  only after a short write. Idle keep-alive connections are never timed out.
- **Fast COBOL.** GnuCOBOL turns `COMPUTE` into GMP decimal arithmetic, so the hot path avoids
  it: indexes and lengths are `COMP-5` binary fields changed with `ADD`/`SUBTRACT`/`MOVE` (which
  compile to plain C), bytes are compared with one-byte reference modification or loaded into a
  binary field through a `REDEFINES`, base64 uses precomputed per-sextet tables instead of
  shifts, and numbers are formatted with one numeric `MOVE`. GnuCOBOL 3.2 is built from source
  rather than using Ubuntu's 3.1.2 because 3.2 compiles reference-modified `MOVE`s to `memmove`
  where 3.1.2 calls the generic `cob_move`. Compiled with `-O2 -fnotrunc -fstatic-call`.
- **JWT**: HS256 only, signature checked on every request (no caching), required `exp` and optional `nbf`
  checked against the clock, `sub` must be a positive integer string and `username` a string.
- `RLIMIT_NOFILE` soft limit is raised to the hard limit; `SIGPIPE` is ignored.

## Limitations

- Body trimming removes ASCII whitespace (space, `\t`, `\n`, `\v`, `\f`, `\r`), not the full
  JavaScript `String.trim` set. Length is counted in Unicode code points.
- Chunked request bodies are not supported (the connection is closed). Requests are capped at
  64 KB.
- Lone UTF-16 surrogates in `\u` escapes are stored as U+FFFD.

## License

MIT, under the repo's [license](../../LICENSE).
