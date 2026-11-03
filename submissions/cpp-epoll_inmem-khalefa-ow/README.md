# C++ + raw epoll, v2

A second version of [`cpp-epoll-khalefa-ow`](../cpp-epoll-khalefa-ow): the same server, tuned for
tail latency and per-request kernel time.

| | |
|---|---|
| Language | C++17, built with Ubuntu 24.04's GCC (`build-essential`) at `-O2 -march=native -flto` |
| Framework | none: a hand-written HTTP/1.1 server on Linux `epoll` (~1,300 lines, `src/main.cpp`) |
| SQLite | the official SQLite **3.53.4** amalgamation, downloaded by `build.sh` (sha256-pinned) and compiled into the binary |
| Crypto | OpenSSL `libcrypto` (Ubuntu's `libssl-dev`) for SHA-256 |
| JSON | hand-written strict RFC 8259 parser and writer |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself (it works behind Nginx on `127.0.0.1:3000` too) |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, libssl-dev, curl
bash build.sh          # downloads + verifies SQLite, builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Changes from v1

- **Exclusive locking mode.** There is one connection, so nothing else needs the database.
  `locking_mode=EXCLUSIVE` takes the file locks once and keeps them, instead of `fcntl`
  lock/unlock calls on every transaction, and keeps the WAL index in heap memory instead of a
  shared `-shm` mapping. This replaces v1's background checkpoint thread, since exclusive mode
  allows only one connection.
- **Checkpoints between batches, not inside a commit.** SQLite's auto-checkpoint would run inside
  the commit that crosses the threshold, so that write's response would wait for the page copying
  and both `fsync`s. A WAL hook records the WAL size instead, and the event loop runs the
  checkpoint (1,000 frames, SQLite's default) after the current batch of responses has been sent.
  Only requests that are already queued wait for it.
- **Small page cache: 500 pages instead of 32 MiB.** Reads go through `mmap`, so the cache only
  holds dirty pages and pages read from the WAL. SQLite scans the cache's whole hash table on every
  commit (`pcache1TruncateUnsafe`), which with 32 MiB was ~13% of user CPU in the request mix.
- **Faster JSON escaping.** The writer scans 16 bytes at a time (SSE2; 8 bytes at a time
  elsewhere) for the bytes that need escaping, which post bodies almost never contain: ~41 ns
  instead of ~300 ns for a 300-byte body, so ~5 µs less per `/feed`.
- **Prewarm at startup.** Every table and index is read once through SQLite's `mmap`, so the file
  is in the OS page cache and mapped before the first request, and early requests don't stall on
  page faults. It takes well under a second. The pages belong to the OS page cache (rule 5), not to
  the server. This is why RSS reads ~220 MB: that is the mapped file (`RssFile`), which the kernel
  can reclaim, and anonymous memory stays around 1 MB.
- **Bounded accepts.** At most 64 new connections are accepted per loop iteration, so a ramp-up
  burst of thousands of connections can't hold up every request behind it.
- **`SQLITE_THREADSAFE=0`.** The server is now single-threaded, so SQLite needs none of its
  mutexes.
- **`STALL_LOG_MS`** (optional, off by default): logs every event-loop batch slower than this many
  ms. Each line gives the batch's wall and CPU time, how the time split between serving requests
  and the checkpoint, and the accepts, closes, page faults and context switches it took.

## Everything else, as in v1

- **One thread, one epoll loop, SQLite called inline.** The box has one vCPU, so extra workers only
  add context switches and locking. Every query is an index or primary-key lookup that takes a few
  microseconds, so running it on the loop costs less than handing it to another thread.
- **Direct instead of Nginx.** On one core, Nginx's 14–20% CPU comes straight out of the app's
  budget, and Nginx's `worker_connections 16384` also caps the user count. An idle keep-alive
  connection is a small struct with empty buffers. Idle connections are closed after 120 s (the
  spec asks for at least 65 s).
- **Zero-copy request parsing.** Each `recv` lands in one shared 64 KiB buffer, and complete
  requests (pipelined ones too) are parsed straight out of it. Only a partial request is copied into
  the connection. All responses produced by one read go out in a single `send`.
- **SQLite built for this program**: `DEFAULT_MEMSTATUS=0` and the usual `OMIT_*` options for
  unused features, compiled together with the server under LTO.
- **Prepared statements** (`SQLITE_PREPARE_PERSISTENT`), prepared once at startup. They cache the
  query plan, not the data. A like is one
  `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`, and only when it inserts nothing does
  a second lookup tell "already liked" (200) from "no such post" (404).
- **Pragmas**: `locking_mode=EXCLUSIVE`, `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB
  `mmap_size`, 500-page cache. Each write is an autocommit statement that is stepped to completion
  (committed) before the response is built.
- **JWT verified on every request** (rule 5): HS256 only, constant-time signature compare, `exp`
  and `nbf` checked. The HMAC key's inner and outer pads are hashed once at startup, so each
  verification is just two SHA-256 passes over the token.
- **Responses are written by hand** into reused buffers: no allocation per request in the steady
  state, and no JSON library.

## Behaviour notes

- Bodies are trimmed with JavaScript's `String.prototype.trim()` whitespace set, and their length is
  counted in Unicode code points (as SQLite's `length()` does).
- Invalid UTF-8 or lone surrogates in a request body count as malformed JSON.
- Chunked request bodies are supported. Requests with headers over 64 KiB or bodies over 1 MiB are
  rejected, and the connection is closed.

## License

MIT, under the repo's [license](../../LICENSE).
