# C++ + raw epoll

| | |
|---|---|
| Language | C++17, built with Ubuntu 24.04's GCC (`build-essential`) at `-O2 -march=native -flto` |
| Framework | none: a hand-written HTTP/1.1 server on Linux `epoll` (~1,200 lines, `src/main.cpp`) |
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

## Optimizations, and why

- **One thread, one epoll loop, SQLite called inline.** The box has one vCPU, so extra workers only
  add context switches and locking. Every query is an index or primary-key lookup that takes a few
  microseconds, so running it on the loop costs less than handing it to another thread. A single
  connection also means writes never wait on SQLite locks.
- **Direct instead of Nginx.** On one core, Nginx's 14–20% CPU comes straight out of the app's
  budget, and Nginx's `worker_connections 16384` also caps the user count. Keep-alive connections cost
  the server almost nothing: an idle connection is a small struct with empty buffers. In a local test
  it held 15,000 idle keep-alive connections while serving ~10k req/s with 0 errors. That test was
  limited by the Node load generator, not the server. Idle connections are closed after 120 s (the
  spec asks for at least 65 s).
- **Zero-copy request parsing.** Each `recv` lands in one shared 64 KiB buffer, and complete
  requests (pipelined ones too) are parsed straight out of it. Only a partial request is copied into
  the connection. All responses produced by one read go out in a single `send`.
- **Background WAL checkpoints.** A second thread with its own connection runs a `PASSIVE`
  checkpoint every 200 ms, so copying pages back into the database and the `fsync` that needs mostly
  happen off the event loop. SQLite's default auto-checkpoint stays on as a backstop. Without it, a
  steady stream of writes would keep the background checkpoint from ever reaching the end of the WAL,
  and the WAL would grow without bound. With the backstop, the WAL stayed around 4 MiB at ~1,000
  writes/s.
- **SQLite built for this program**: `SQLITE_THREADSAFE=2` (no per-call mutexes, since each
  connection is used by one thread), `DEFAULT_MEMSTATUS=0`, and the usual `OMIT_*` options for unused
  features, compiled together with the server under LTO.
- **Prepared statements** (`SQLITE_PREPARE_PERSISTENT`), prepared once at startup. They cache the
  query plan, not the data. The SQL is the reference queries. A like is one
  `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`, and only when it inserts nothing does
  a second lookup tell "already liked" (200) from "no such post" (404).
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 32 MiB page
  cache. Each write is an autocommit statement that is stepped to completion (committed) before the
  response is built.
- **JWT verified on every request** (rule 5): HS256 only, constant-time signature compare, `exp`
  and `nbf` checked. The HMAC key's inner and outer pads are hashed once at startup, so each
  verification is just two SHA-256 passes over the token. That is key setup, not caching a result.
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
