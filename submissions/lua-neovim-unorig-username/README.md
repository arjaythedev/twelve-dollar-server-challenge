# lua-neovim-unorig-username

The web server is a text editor. `start.sh` runs `nvim --headless --clean -l server.lua` and nothing else:
no Nginx, no helper daemon, no compiled code of ours. One file of Lua, about 300 lines.

- **Language / runtime:** Lua on the LuaJIT 2.1 that is built into Neovim 0.9.5 (Ubuntu package `neovim=0.9.5-6ubuntu2`)
- **Network:** `vim.loop` (Neovim's own libuv 1.48 event loop, through luv)
- **SQLite:** Ubuntu's `libsqlite3.so.0` (`libsqlite3-0`, 3.45.1), called through LuaJIT's FFI
- **JWT:** `HMAC()` from the system `libcrypto.so.3` through the FFI; JSON through Neovim's `vim.json`
- **Mode:** direct. Neovim listens on `HOST:PORT` itself (`0.0.0.0:80` on the box), no Nginx.

## How it works

- A libuv TCP server accepts connections inside the editor process. Each read callback parses as many
  complete HTTP/1.1 requests as the buffer holds (pipelining and split requests both work) and builds
  all their responses in one FFI byte buffer.
- Every request runs its query on one SQLite connection with reused prepared statements. Columns are copied
  from SQLite's memory straight into the response buffer (JSON-escaped on the way), so a feed costs
  one Lua string. Nothing is cached between requests, and every token gets a fresh HMAC-SHA256.
- **Group commit:** the first write in a turn of the event loop opens a transaction. A libuv check handle
  runs right after the loop's I/O phase: it commits, and only then sends that turn's responses, one
  `try_write` per connection. A 201 never leaves before its `COMMIT` (WAL, `synchronous=NORMAL`).
  If a commit fails, the connections of that turn are closed without an answer.
- **More than 65,535 connections:** a process stops at 60,000 connections, starts a fresh Neovim that
  inherits the listening socket as fd 3, and keeps serving what it has. A child that has handed the
  socket on exits once its last connection is gone. All processes share the one vCPU.
- **It is still an editor.** Buffer 1 holds a live counter line, refreshed once a second. Ask the running
  server for it from another shell (the socket is under `${XDG_RUNTIME_DIR:-/tmp/nvim.$USER}`):

  ```
  $ nvim --server /tmp/nvim.runner/Nkqvqc/nvim.15.0 --remote-expr 'getline(1)'
  uptime 2 s, 0 connections, 1 requests
  ```

HMAC choice: the FFI call costs about 1.6 us per token, and only about 8% of the load test's requests
carry a token, so a pure Lua SHA-256 had nothing left to win.

Not handled (the spec does not ask for them): `Transfer-Encoding: chunked`, `Expect: 100-continue`,
bodies on GET requests, write backpressure for a client that pipelines without reading.

## Numbers (local, rough)

Measured on a laptop, not on the droplet: the server in a 1-CPU, 2 GB, `nofile=65535` x86_64 container
(emulated on an ARM host), k6 in separate containers on the same machine (20,000 users each), which was
also busy with other work. `bench/load.js`, 60 s ramp, 2 min hold.

| k6 users | req/s | p95 | p99 | failed | server CPU | server memory |
|---|---|---|---|---|---|---|
| 10,000 | 830 | 0.4 ms | 0.9 ms | 0% | 11% | 70 MB |
| 40,000 | 3,330 | 0.5 ms | 3.6 ms | 0% | 27% | 160 MB |
| 80,000 | 6,650 | 2.1 ms | 11 ms | 0% | 65% | 300 MB |
| 120,000 | 9,950 | 33-47 ms | 285-720 ms | 0% | 85-90% | 450 MB |
| 140,000 | 11,600 | 168 ms | 705 ms | 0% | 100% (saturated) | 470 MB |

The CPU is full at 140,000 users, so p99 should cross 1 s just above that (not measured further).
Up to 60,000 users it is one process; the 80,000+ rows include the handoff to a second and third Neovim.

Without think time (wrk, 100 feed : 100 post : 15 like : 2 create): about 40,000 req/s at 64 connections,
26,000 req/s on a writes-only run.

`test/run.sh`: 42 passed, 0 failed.
