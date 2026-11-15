# prolog-swipl-unorig-username

The web server is a logic program. Every route is a clause of `route/4`, the request body is parsed by a
DCG, and a like is only written once Prolog has proved you hold a valid token. `start.sh` runs
`swipl -O server.pl`, with Nginx in front.

| | |
|---|---|
| Language | Prolog (`server.pl`, about 330 lines) |
| Runtime | SWI-Prolog 9.0.4, Ubuntu package `swi-prolog-nox 9.0.4+dfsg-3.1ubuntu4` (pinned) |
| HTTP | our own HTTP/1.1 loop on `library(socket)`: `wait_for_input/3` over all connections, one thread |
| SQLite | system SQLite 3.45.1 (`libsqlite3-dev`), in-process through `sqlite4pl.c`, a three-predicate foreign library that `build.sh` compiles with `swipl-ld` |
| JSON | a strict RFC 8259 parser written as a DCG for requests; SQLite's `json_object` for post objects |
| JWT | HS256 with `hmac_sha/4` (`library(sha)`), base64url with `base64_encoded/3` |
| Front | **behind Nginx** (`bench/nginx.conf`), listening on `127.0.0.1:3000` |

## The honest part

SWI-Prolog has no SQLite in its distribution, so there is a little C: `sqlite4pl.c` (about 70 lines) gives
Prolog `sqlite_open/1`, `sqlite_prepare/2` and `sqlite_run/3` (bind, step once, reset). It knows nothing about
posts, HTTP or JSON. Routing, HTTP parsing, auth, validation, the group commit and every status code are Prolog.
The post objects themselves are assembled by SQLite's `json_object`, so a feed comes back from the database as
one string; Prolog wraps it and counts its bytes.

## How it works

- **One process, one thread, one event loop.** `wait_for_input/3` waits on the listening socket and every
  connection. A readable connection is drained with `fill_buffer/1` and `read_pending_codes/3` (octet streams),
  and every complete request in the buffer is answered, so pipelining and split requests both work. A partial
  request waits in the clause database until the rest arrives. Connections are never closed by the server.
- **Failure-driven loop.** `main` is `repeat, turn, fail`: every turn starts with empty Prolog stacks, so the
  garbage of a turn is dropped by backtracking instead of by the garbage collector. What must survive a turn
  (open connections, partial requests, prepared statements) lives in dynamic predicates.
- **Routes are clauses.** The path is split on `/` and matched by `route(Method, Segments, Request, Response)`:
  `route("GET", ["posts", Id], ...)`, `route("POST", ["posts", Id, "like"], ...)` and a last clause for 404.
  Authenticated routes check the token before the id.
- **Every request reads SQLite.** One prepared statement per request kind, reused (only the plan is cached).
  The feed is one statement: `group_concat(json_object(...) ORDER BY created_at DESC, id DESC)` over the 20 newest.
- **Every token is verified.** HMAC-SHA256 over `header.payload` per request, `alg` must be `HS256`, `exp` is
  required and must be in the future, `nbf` is honoured if present, `sub` must be a positive integer string,
  `username` a string.
- **Strict JSON.** The DCG takes RFC 8259 and nothing else: no `NaN`, `01`, `+1`, `1.`, comments, trailing
  commas, raw control characters, bad escapes or trailing garbage. `\u` escapes and surrogate pairs decode
  to Unicode; a repeated key takes the last value.
- **Group commit.** The first write of a turn opens `BEGIN IMMEDIATE`; at the end of the turn one `COMMIT`
  runs and only then are the turn's responses written. A 201 never leaves before its commit (WAL,
  `synchronous=NORMAL`). If the commit fails, the clients of that turn get no answer at all.
- Ids: ASCII digits only, leading zeros allowed, `0` is invalid, 19 digits and more is a post that does not exist.

## Why not `library(http/thread_httpd)`

Measured first, in the same 1-CPU x86_64 container, `wrk -c64`, `GET /posts/12345`: `thread_httpd` with
`http_handler/3` routes and 4 workers served 5,100 req/s for a handler that does less than ours; the
hand-written loop above serves 34,000 req/s. On one vCPU the worker threads only add switching.

## Why Nginx

`wait_for_input/3` is `poll()`, so it has no 1,024 limit, but the loop builds the list of all connections
and polls all of them on every turn. With 5,000 idle keep-alive connections held open, the same probe lost
about 40% of its throughput (13,000 to 7,500 req/s, native laptop run). Direct on port 80 means ~15,000
connections; behind Nginx it is a few dozen.

## Numbers (local, rough)

Not the benchmark box: the server and Nginx (1 worker) in one 1-CPU x86_64 container (emulated on an ARM
laptop VM shared with other work), k6 `bench/load.js` in separate containers. Earlier entries ran 1.3x to
2.5x higher here than on the droplet.

| VUs | ramp, hold | req/s | p95 | p99 | failed | container CPU | RSS swipl / Nginx |
|---|---|---|---|---|---|---|---|
| 10,000 | 30 s, 20 s | 682 | 2.5 ms | 8.1 ms | 0% | ~25% | 144 MB / 89 MB |
| 25,000 | 30 s, 45 s | 1,907 | 4.9 ms | 21 ms | 0% | 60-65% | 145 MB / 89 MB |
| 36,000 | 60 s, 60 s | 2,688 | 6.4 ms | 75 ms | 0% | 80-90% | 148 MB / 89 MB |
| 42,000 | 60 s, 60 s | 3,134 | 5.1 ms | 22 ms | 0% | 80-94% | 151 MB / 89 MB |

At 42,000 the CPU is close to full, so the local limit is not far above that. Per request in the loop
(profiled): about 5 us of Prolog and 5 us of SQLite for a `GET /posts/:id`; a feed is mostly SQLite's
20 `count(*)` subqueries.

`test/run.sh`: 42 passed, 0 failed (Ubuntu 24.04 x86_64, `install.sh` as root, SWI-Prolog 9.0.4).

## Not handled

The spec does not ask for them: `Transfer-Encoding: chunked`, `Expect: 100-continue`, `Connection: close`
(the client closes), bodies on `GET`, write backpressure for a client that pipelines without reading, and
lone UTF-16 surrogates in request JSON (rejected as malformed). A request line that does not parse, or an
unfinished request over 64 KB, closes that connection.
