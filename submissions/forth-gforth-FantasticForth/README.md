# forth-gforth

A from-scratch implementation of the $12 Server Challenge feed API in **Forth**.

## Stack

| | |
|---|---|
| **Language** | Forth (ANS/Gforth dialect) |
| **Runtime** | Gforth 0.7.3 (Ubuntu 24.04 `apt` package) |
| **HTTP** | Hand-written HTTP/1.1 server (no framework) on a single-process **epoll** event loop, raw BSD sockets via Gforth's C FFI |
| **SQLite driver** | Direct FFI to the system `libsqlite3` (prepared statements, no ORM) |
| **Crypto** | Direct FFI to `libcrypto` (OpenSSL) `HMAC` + `EVP_sha256` for JWT HS256 |
| **Nginx or direct** | **Direct** — serves the internet itself on `0.0.0.0:80` (env-overridable; set `HOST=127.0.0.1 PORT=3000` to run behind Nginx) |
| **Versions** | Pinned to the Ubuntu 24.04 (Noble) archive: Gforth `0.7.3+dfsg-9build4.1`, libsqlite3-dev `3.45.1-1ubuntu2.8`, libssl-dev `3.0.13-0ubuntu3.16`, libtool-bin `2.4.7-7build1`, build-essential `12.10ubuntu1`. `install.sh` installs these via `apt` on the fixed distro release (the distro release is the pin); it does not download or build any external dependency. |

There is no third-party Forth library here: SQLite, HMAC-SHA256, and the sockets are
all reached through Gforth's built-in `c-library` FFI, which compiles a small C
wrapper to a shared object at build time.

## Files

| File | Role |
|---|---|
| `ffi.fs` | The single shared `c-library` FFI block: SQLite, OpenSSL HMAC, and socket syscalls + small C helpers |
| `db.fs` | SQLite layer: open (WAL, `synchronous=NORMAL`), prepared statements, bind/step/column accessors |
| `jwt.fs` | JWT HS256 verification: base64url, `alg` check, constant-time signature compare, `exp` check, payload validation |
| `json.fs` | Compact exact-key-order JSON emitter + a minimal request-body parser |
| `http.fs` | HTTP/1.1 request parsing, routing for the 5 endpoints, the error contract, and response building (shared by the event loop) |
| `epoll.fs` | Single-process epoll event loop: non-blocking sockets, connection slot pool, accept backpressure, connection-pressure shedding, group commit + off-path WAL checkpointing; drives the `http.fs` pipeline |
| `server.fs` | Loads every layer and calls `epoll-main` |
| `compile-check.fs` | Loads every layer without starting the server (used by `build.sh`) |

## How it runs

- `install.sh` (root, once): `apt-get install gforth build-essential libtool libsqlite3-dev libssl-dev`.
- `build.sh` (user): runs `gforth compile-check.fs`, which forces Gforth to compile the FFI
  wrapper `.so` and loads all five layers. Fails fast if the FFI link or any definition is broken.
- `start.sh` (user): `exec gforth server.fs`. Reads `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`
  from the environment and serves in the foreground.

## Design notes & optimizations

- **Prepared statements, reused.** The six statements (feed, post-by-id, health `SELECT 1`,
  insert post, insert like, post-exists) are prepared once at startup and `reset`+`clear_bindings`
  per request. This caches the query plan, not data — every request still reads live rows
  (rule 5). `like_count` is a correlated subquery evaluated per request.
- **The feed query is left as-is on purpose.** The `like_count` correlated subquery looks like
  the obvious thing to "fix", but profiling says otherwise: the prepared query runs in ~23 µs
  warm (the subquery itself is only ~10 µs — 20 lookups against the covering `likes_post_id_idx`),
  because `posts_created_at_id_idx` already yields rows in `created_at DESC, id DESC` order and the
  planner stops at `LIMIT 20`. The tempting `LEFT JOIN … GROUP BY` rewrite is ~2,600× *slower*
  here: it forces a full scan of `posts` and a temp B-tree for the ordering. So the SQL is kept.
- **JSON string fast path.** Profiling showed ~90% of a `/feed` request's CPU was building the
  ~5 KB JSON response one byte at a time (each append was a separate interpreted word). `json-str`
  now bulk-copies maximal runs of "safe" bytes with a single `move`, dropping to per-character
  escaping only for the bytes that actually need it (`"`, `\`, and control bytes `< 0x20`). The
  output is byte-for-byte identical (the golden `/feed` read still matches), and this is pure
  response formatting — it caches nothing and every request still reads live rows (rule 5). It
  roughly halved per-request CPU (~380 µs → ~195 µs), about doubling single-core `/feed` throughput.
- **JSON scan + integer fast paths.** A second profiling pass (perf over a saturated 5-min run,
  plus isolated gforth microbenchmarks) showed that after the bulk-`move` change, response
  formatting was still the single largest cost — ~64% of a warm `/feed` (`~58 µs` of `~90 µs`),
  larger than the SQLite query itself (`~28 µs`). Two changes cut that:
  - The safe-byte scan (`json-run-len`) now indexes a 256-entry `json-safe` lookup table (one
    load + branch per byte) instead of calling a word that did three comparisons. The table *is*
    `json-needs-esc?` inverted, so the escaping contract (`"`, `\`, control `< 0x20`) is unchanged.
  - `json-int` no longer uses pictured numeric output (`<# #s #>`) plus a `move`; it writes decimal
    digits straight into a scratch buffer and copies the run once. Per-digit cost dropped from
    ~48 ns to ~25 ns (a `/feed` emits 40 integers).
  Together these took the JSON build from ~58 µs to ~37 µs (−36%), the full `/feed` handler from
  ~90 µs to ~68 µs (−24%), and single-core throughput behind Nginx from ~6,800 to ~8,000 req/s
  (+18%) on a 1 vCPU / 2 GB box. Output is byte-for-byte identical (verified against reference
  emitters across quote/backslash/control/multibyte cases and the full `test/test.sh` golden reads);
  nothing is cached (rule 5).
- **Durability (rule 6).** The database is opened with `PRAGMA journal_mode=WAL` and
  `PRAGMA synchronous=NORMAL`; a `201`/`200` write response is only sent after its transaction
  has committed (see group commit below).
- **SQLite pragmas (rule 6 / rule 11 — all in-process).** `locking_mode=EXCLUSIVE` (single
  connection, so no per-transaction lock syscalls and simpler WAL coordination),
  `cache_size=-65536` (64 MiB of hot b-tree pages in SQLite's `pcache1`), `temp_store=MEMORY`, and
  `journal_size_limit=64 MiB`. **`mmap_size=0` on purpose:** profiling showed that in WAL mode a
  non-zero mmap routes page fetches through `getPageMMap`, which bypasses `pcache1` and forces a
  WAL hash-segment probe on every b-tree lookup — it made `/feed` ~2 µs/request *slower* than
  reading through `pcache1` plus the OS page cache. 64 MiB of cache profiled the same as 256 MiB
  (the OS page cache holds the rest of the 238 MB db), so we take the smaller footprint on the
  2 GB box. These cache nothing at the application level — SQLite's own page cache is explicitly
  allowed (rule 5).
- **Off-request-path WAL checkpointing (the WAL-growth cliff).** `wal_autocheckpoint=0` turns off
  SQLite's automatic checkpointing. Left unmanaged under sustained writes the WAL grows without
  bound and every read slows down (each page lookup probes the WAL hash segments). Instead the
  event loop checkpoints *itself*, off the request path: after a write batch commits (nothing is
  writing at that instant), once ~1,000 writes have accumulated it resets its read statements
  (an un-reset stepped `SELECT` on the single EXCLUSIVE connection otherwise holds a read
  transaction open and makes the checkpoint return `SQLITE_LOCKED`) and runs a `TRUNCATE`
  checkpoint. This bounds the WAL to a few MB and keeps `/feed` fast under a sustained write load,
  while amortizing the checkpoint fsync across many writes rather than paying it per batch.
- **Group commit (rule 6).** All writes produced during one `epoll_wait` batch share one
  `BEGIN IMMEDIATE … COMMIT`, opened lazily on the first write of the pass and committed once at
  the end of the pass. Crucially, a write's response bytes are *built and armed* during the pass
  but only *sent* on a subsequent `epoll_wait` iteration — which is after the end-of-pass commit —
  so a `201` is never emitted before its row is durably committed. If the commit fails, every
  pending write in that batch is converted to a `500` before anything is sent, and the
  transaction is rolled back.
- **No `RETURNING` on insert.** `POST /posts` uses a plain `INSERT INTO posts …` then reads the
  new id with `sqlite3_last_insert_rowid()` and the stored `created_at` with a prepared
  `SELECT created_at FROM posts WHERE id=?`. This avoids the ephemeral b-tree and second step that
  `INSERT … RETURNING` adds to the write path.
- **Fresh JWT verification per request.** Every token's signature is recomputed with
  `HMAC`/`EVP_sha256` and compared in constant time; nothing about a token is remembered
  between requests (rule 5). `alg` must be exactly `HS256` (so `alg:none` is rejected), and
  `exp` is checked against the current time.
- **Zero-copy-ish response building.** JSON is appended into a single reusable buffer and the
  response (status line + headers + body) is written into a per-connection write buffer and
  drained with non-blocking `send` (`MSG_NOSIGNAL`).
- **Keep-alive.** Connections are kept open across requests. `TCP_NODELAY` is set to avoid Nagle
  latency on small JSON responses. In direct mode each client holds its own keep-alive connection;
  the server manages (and sheds) these itself — see connection shedding below.
- **Concurrency model: single-process epoll event loop.** `epoll.fs` runs one `epoll_wait`
  loop over non-blocking sockets (`server.fs` calls `epoll-main`). Each connection owns a slot
  in a fixed pool (`MAX-SLOTS`, 20,000) with its own read/write buffers; slots are handed
  out and returned through an O(1) free-list stack, and an `fd -> slot` map (bounds-checked) routes
  events. A request is parsed once its headers + `Content-Length` body have fully arrived, routed
  through the same `parse-request`/`route` pipeline as before, and the response is streamed back
  without blocking the loop. At ~17,000 live connections the resident set is ~220 MB (the buffer
  pool is lazily faulted, ~8 KB per live connection), comfortably inside the 2 GB box.
- **Connection shedding (passing the fd ceiling honestly).** In direct mode the binding limit is
  the slot pool / the box's `LimitNOFILE=65535`, not CPU — under the think-time load the core sits
  at 16–29 % at 10–20 k users. The server tracks its open-connection count in O(1)
  (`MAX-SLOTS - free-top`, no syscalls). Past a soft threshold (85 % of the pool) it answers
  *finishing* connections with `Connection: close` so those clients cycle out and free their slot
  for a waiting client (one cheap `accept` on their next request instead of a permanently pinned
  fd); past a hard threshold (97 %) every finishing connection closes; it returns to normal
  keep-alive once the count falls below a lower watermark (70 %). The decision is made **per
  connection as its request completes** (deciding per request-in-flight made the count hover at
  the cap and doubled the churn — the same thing the asm/epoll entries report). This keeps only
  transport metadata; every request still reads live SQLite (rule 5). Under a sustained 35 k-user
  direct load the open count oscillates in a stable 14 k–17 k band (hysteresis working) at 0 %
  errors.
- **Accept backpressure (no overload runaway).** When the slot pool is full, or `accept()` fails
  because the process is out of file descriptors (`EMFILE`/`ENFILE`), the server *disarms the
  listen socket* (`EPOLL_CTL_MOD` to interest 0) instead of accepting-and-closing in a tight loop.
  With a level-triggered listen fd an eager client (k6/Nginx) that reconnects instantly would
  otherwise pin a core at 100%. Accepting resumes automatically when a slot frees. The kernel
  listen backlog (4096) absorbs bursts while paused. This keeps CPU, memory and fd use bounded
  under offered load far above capacity rather than exhausting the box.
- **File-descriptor limit.** `start.sh` raises `RLIMIT_NOFILE` to the hard limit (the benchmark
  box grants `LimitNOFILE=65535`). In direct mode every keep-alive client is an fd, so without
  this the default soft limit (often 1024) would cap concurrent connections far below the pool.
  Resident memory is ~8 KB per live connection (buffers are lazily faulted), so the pool's
  practical ceiling (~17 k connections under shedding) costs ~220 MB — far inside the 2 GB box.

## Correctness

Passes all 42 checks in `test/test.sh` (byte-exact golden reads, the full error contract
including auth-before-id ordering and `alg:none` rejection, body trimming and the 500-char
limit, and UTF-8 round-tripping).
