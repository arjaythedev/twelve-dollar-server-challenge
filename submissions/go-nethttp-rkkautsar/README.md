# Go custom HTTP + direct SQLite

**Language:** Go 1.27.1
**Framework:** custom HTTP/1.1 parser on a `bufio` connection loop; `net/http` only for status constants and optional pprof
**Driver:** direct cgo against the vendored SQLite 3.46.1 amalgamation (`sqlite3.c`/`sqlite3.h`), built with `SQLITE_THREADSAFE=2` and `SQLITE_OPEN_NOMUTEX`
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx.

Five endpoints. Concurrent requests for the same feed or post are pooled into one in-flight
SQLite read and share its result; the group is deleted as soon as it completes. Nothing is
cached after a group finishes, so every request group reads its data from SQLite while it is
being served.

## Implementation notes

- **Split read/write paths.** Reads use a pool of 2 connections (the measured sweet spot on
  1 vCPU; `READ_CONNS` overrides it). Writes use one dedicated connection and a writer
  goroutine that group-commits queued create/like jobs every `WRITE_BATCH_MS` (default 100 ms).
  Every write response waits for that commit, so a 201/200 is only sent for durable rows.
  Rule 6 explicitly allows group commit under that condition.
- **Raw io_uring request mux (`URING=1`, opt-in).** One event loop owns the
  ring, registered provided-buffer ring (`IORING_REGISTER_PBUF_RING`), fd table,
  HTTP parser, output queues and raw `IORING_OP_SEND` submissions. One DB
  owner goroutine executes the blocking SQLite handler; `/feed` requests are
  batched per event-loop turn and share one query result. There is no goroutine
  per connection and no per-request transport goroutine. Multishot receives re-arm only when
  `IORING_CQE_F_MORE` is absent or on `-ENOBUFS`; consumed buffers are returned
  by advancing the buffer-ring tail. There is no `iouring-go` dependency and no
  vendor tree. The Go connection loop remains the default: it is faster for the
  current blocking SQLite handler, and the mux falls back to it on setup failure.
- **In-flight read coalescing.** A request for `/feed` or `/posts/:id` joins the current
  group for that key instead of issuing its own query. The key includes a generation counter
  bumped after every successful commit, so a request that starts after a 201/200 can never
  join a read that began before that write. Completed groups are removed immediately; there
  is no time-based or cross-request cache.
- **Read batch window.** A read leader waits `READ_BATCH_US` (default 500 µs) before
  querying, so concurrent requests can join the group. On one vCPU an immediate leader
  finishes before another goroutine gets CPU, which made coalescing almost useless (measured
  group size ≈1.02). With the window at 100 clients: `/feed` went from 7.6k to 18.5k req/s,
  server CPU from 124 to 52 µs/request, p95 from 22.9 to 8.8 ms; `/posts/:id` went from 12.0k
  to 23.8k req/s for one hot id and 15.5k to 21.3k req/s across 20 ids. The delay is a
  collection window, not a cache: the group is still deleted as soon as the read completes.
- **Minimal HTTP parser.** The connection loop parses the request line and only the headers
  the API uses (`Authorization`, `Content-Length`, `Connection`) into a stack-sized struct,
  instead of `net/http` building a canonicalized header map. It accepts the Content-Length
  requests sent by curl/k6; chunked bodies and other methods are outside the tested surface.
  At 100 clients this raised `/feed` from 17.3k to 18.9k req/s (56 → 49 µs CPU) and
  `/posts/:id` from 20.6k to 24.4k req/s (44 → 36 µs). All exact-byte tests pass.
- Each connection runs `PRAGMA busy_timeout=5000`, `journal_mode=WAL`,
  `synchronous=NORMAL`, `cache_size=-16384` (16 MiB) and `mmap_size=268435456` (256 MiB).
  The cache and mmap are SQLite's own page cache, which the rules allow.
- All queries are prepared once at startup and reused. Prepared statements cache the
  query plan, not row data.
- `like_count` is counted from `likes` in the request itself with a correlated subquery; no
  counter table or trigger is used, and the schema is unchanged.
- Feed and single-post reads use SQLite's JSON functions: `json_group_array(json_object(...))`
  and `json_object(...)` return the complete response fragment as one TEXT column. Go appends
  that one column to `{"posts":`/`{"post":` and closes the object, instead of making five cgo
  column calls per row and hand-encoding every field. SQLite is still computing the JSON for
  each request; nothing is cached. The exact-byte test suite passes, including escaping.
- On the local 1-CPU pin, 100 keep-alive clients, `/feed`: Go field encoding used
  127–137 µs of server CPU per request; SQLite JSON used 117–122 µs (about 8% less).
- Against the previous committed build (`database/sql` + mattn + Go encoding), same pin and
  100 clients, `/feed`: 144–152 µs CPU/request and 6.3–7.0k req/s; before the batch window
  this build measured 120–123 µs and 8.1–8.3k req/s. With the 500 µs window it measures about
  52 µs and 18.5k req/s; the minimal parser brings it to ~49 µs and ~18.9k req/s.
- Prebuilt packets measured throughput-neutral within run variance (feed ~20.5k, post
  ~27–28k req/s; CPU/request unchanged) and removed the per-response 5 KB buffer copy and
  most of the per-connection write buffer. Struct-map keys were also measured and gave no
  gain, so string keys remain.
- CPU profile with the window and minimal parser, `/feed` at 18.0k req/s: socket writes
  ~67%, socket reads ~18%, SQLite `step` ~9.4%. For `/posts/:id` with the window the same
  shift applies; without it, SQLite `step` was ~51%. SQLite is no longer the bottleneck;
  kernel socket I/O is.
- JWT HS256 verification is implemented directly with `crypto/hmac`; only `HS256` is accepted
  and `exp` is checked on every request (no token cache). Claims are decoded into a struct.
- Write/error responses are hand-encoded into byte buffers with fixed key order and JSON string
  escaping. Single-request responses use a pooled buffer transferred to the connection writer
  (no copy) and returned after the flush; coalesced read responses use an immutable body shared
  by the group. The connection loop uses a 2 KiB read buffer. Coalesced feed/post responses
  are built once per group as a complete
  immutable packet (status line + headers + body) and written with one `Write`; the
  per-connection `bufio.Writer` is only 1 KiB for write/error responses. At 15k connections
  that is ~75 MB less write-buffer memory than the previous 6 KiB buffer. `TCP_NODELAY` is
  set explicitly.
- Keep-alive is the HTTP/1.1 default; idle connections are not closed.
- `PPROF_ADDR` optionally starts a `net/http/pprof` listener; it is unset in benchmark runs.

## Local measurements

`dev/loadtest.sh` runs both the server and k6 inside Docker Desktop's VM, with the server
capped at 1 CPU, 2 GB, no swap and 65,535 open files. 10,000 VUs for 5 minutes:

| Metric | Value |
|---|---:|
| Requests | 373,320 |
| Throughput | 933 req/s |
| p95 / p99 | 42.6 ms / 100.6 ms (100 ms write window) |
| Failed requests | 0 |
| CPU per request | **162.8 µs** |
| Peak server memory | 340 MB |

CPU/request evolution on the same 10k run: 253 µs baseline → 211 µs with the batched writer
→ 167.5 µs with the connection loop → **162.8 µs** after transferring body buffers and
shrinking per-connection buffers (6 KiB write + 2 KiB read). Allocation churn fell from
224 MB to 89 MB per 30 s and live heap from 126 MB to 96 MB; peak RSS dropped 405 → 340 MB.

These are local emulations, not the official benchmark. Above ~10k VUs the rig is limited by
Docker Desktop's VM memory (k6 VU runtimes), not by the server. The numbers above predate the
direct-cgo/coalescing changes and are kept as a baseline; the current build was re-tested for
correctness and throughput separately.

## Scripts

- `install.sh` — Go 1.27.1 toolchain and `build-essential` (cgo); SQLite is vendored, so there is no system SQLite dependency.
- `build.sh` — cgo build of the vendored SQLite amalgamation, trimpath and stripped, to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
- `dev/loadtest.sh` — optional local load rig (see `dev/README.md`).
