# prolog-swipl-unorig-username

The web server is a logic program. Every route is a clause of `route/4`, the request body is parsed by a
DCG, and a like is only written once Prolog has proved you hold a valid token. `start.sh` runs
`swipl -O server.pl`, serving port 80 directly.

| | |
|---|---|
| Language | Prolog (`server.pl`, about 350 lines) |
| Runtime | SWI-Prolog 9.0.4, Ubuntu package `swi-prolog-nox 9.0.4+dfsg-3.1ubuntu4` (pinned) |
| HTTP | our own HTTP/1.1 loop on `library(socket)` and an epoll set (`epoll4pl.c`), one thread |
| SQLite | system SQLite 3.45.1 (`libsqlite3-dev`), in-process through `sqlite4pl.c`, a three-predicate foreign library that `build.sh` compiles with `swipl-ld` |
| JSON | a strict RFC 8259 parser written as a DCG for requests; SQLite's `json_object` for post objects |
| JWT | HS256 with `hmac_sha/4` (`library(sha)`), base64url with `base64_encoded/3` |
| Front | **direct** on `0.0.0.0:80`, no Nginx |

## The honest part

SWI-Prolog has no SQLite in its distribution, so there is a little C: `sqlite4pl.c` (about 70 lines) gives
Prolog `sqlite_open/1`, `sqlite_prepare/2` and `sqlite_run/3` (bind, step once, reset). It knows nothing about
posts, HTTP or JSON. `epoll4pl.c` (about 60 lines) is the other piece of C; see "What is in C" below. Routing, HTTP parsing, auth, validation, the group commit and every status code are Prolog.
The post objects themselves are assembled by SQLite's `json_object`, so a feed comes back from the database as
one string; Prolog wraps it and counts its bytes.

## How it works

- **One process, one thread, one event loop.** `epoll_wait/1` returns the connections that have input. A
  readable connection is drained with `fill_buffer/1` and `read_pending_codes/3` (octet streams), and every
  complete request in the buffer is answered, so pipelining and split requests both work. A partial request
  waits in the clause database until the rest arrives. Connections are never closed by the server.
- **One send per connection per turn.** The responses for a connection are joined into one string and written
  with `fd_send/3`, not through a Prolog output stream. If the socket does not take all of it (a client that
  pipelines and does not read), the rest waits in `pending/2` and that connection is not read again until it
  has gone out, so one such client neither stalls the others nor makes the server buffer without limit.
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

## Direct, and why

Behind Nginx, Nginx used about a third of the CPU per request (about 60 of 180 us locally), and the rig's
`bench/nginx.conf` allows 16,384 `worker_connections`, so with more keep-alive users than that it closes idle
client connections, and a POST that lands on one is lost. So the server now takes port 80 itself and keeps
every connection open until the client closes it.

`wait_for_input/3` is `poll()`, which walks every open connection on every turn: with 5,000 idle keep-alive
connections the loop lost about 40% of its throughput. epoll returns only the ready ones, so 60,000 mostly
idle connections cost the loop nothing.

## What is in C

`epoll4pl.c` gives Prolog four predicates: `epoll_add/1`, `epoll_wait/1` (level-triggered, up to 1,024 ready
descriptors per call), `epoll_watch/2` (wait for input, for room to write, or for neither) and `fd_send/3` (a non-blocking
`send()` that returns what the socket did not take). It parses nothing and decides nothing. Accepting,
reading, HTTP, routing, JWT, JSON, SQL, the group commit and every status code stay in Prolog.

Why `fd_send/3` and not the connection's Prolog output stream: a stream that has written a response keeps its
buffer (64 KB here, 4 KB by default) for as long as the connection is open. With one idle connection per user
that was 69 KB each, 1.6 GB at 24,000 users, and the process was OOM-killed at 30,000. Writing from one
string per turn instead, an idle connection costs about 5 KB (its descriptor, the input stream and its
buffer): measured 16 MB idle, 73 MB with 10,000 and 130 MB with 20,000 connections that had each fetched
`/feed` once.

## Numbers (local)

Not the benchmark box: one core of a shared x86_64 VM (emulated on an ARM laptop), the server's container
capped at 1 ms of every 1.7 ms of CPU, 2 GB, `nofile=65535`. With that cap the merged version behind Nginx
uses about 75% of the cap at 24,000 users. k6 `bench/load.js` from separate containers on other CPUs, 60 s
ramp, 2 minute hold unless noted.

Same load, merged version against direct (merged: range over 4 runs, 2 series of 2 rounds in opposite order;
direct: this version, and in brackets the range over 2 runs of an earlier build of it that did not yet
handle clients that do not read, or check the listening socket every turn):

| Users | version | p95 | p99 | failed | CPU of cap | us/request | RSS |
|---|---|---|---|---|---|---|---|
| 24,000 | merged, behind Nginx | 1.2 to 2.2 ms | 4.1 to 10 ms | 0% (2 to 5 likes) | 72 to 82% | 172 to 197 | 46 to 53 MB |
| 24,000 | direct | 0.6 ms (0.5 to 0.7) | 2.5 ms (1.5 to 3.0) | 0% | 49% (41 to 44) | 120 (105 to 113) | 174 MB |
| 30,000 | merged, behind Nginx | 3.7 to 229 ms | 21 to 2,881 ms | 0 to 0.06% | 94 to 98% | 177 to 186 | 55 to 96 MB |
| 30,000 | direct | 0.7 ms (0.6) | 3.4 ms (2.2 to 2.7) | 0% | 59% (57 to 60) | 118 (113 to 117) | 206 MB |

Behind Nginx, every run lost a few likes to EOFs on connections Nginx had closed (one 30,000 run also lost
377 requests); direct lost none.

Climbing (rows up to 55,000 from the build before the listening socket was checked every turn, which only
matters while users connect faster than about 500 a second):

| Users | req/s | p95 | p99 | failed | CPU of cap | RSS |
|---|---|---|---|---|---|---|
| 40,000 | 3,327 | 1.1 ms | 10 ms | 0% | 76% | 265 MB |
| 50,000 | 4,160 | 1.8 ms | 22 ms | 0% | 87% | 323 MB |
| 55,000 | 4,576 | 2.6 ms | 22 ms | 0% | 92% | 351 MB |
| 60,000 (2 runs) | 4,843 to 4,904 | 1.4 to 2.0 s | 1.9 to 3.9 s | 0% | 98 to 99% | 388 MB |
| 64,000 (2 runs) | 5,312 to 5,318 | 92 to 196 ms | 198 to 312 ms | 0% | 98% | 406 to 413 MB |
| 64,000, 5 minute hold | 5,925 | 362 ms | 2.05 s | 0% | 98% | 421 MB |
From 60,000 the CPU cap is full and requests queue (the median goes from 0.3 ms at 55,000 to 17 ms at 60,000),
so the local limit is CPU, at about 55,000 users with a short tail and 64,000 without errors. Above 65,000
the process would run out of descriptors (`nofile=65535`). About 95 to 120 us of CPU per request.

## Tests

On a fresh ubuntu:24.04 amd64 after `install.sh`: `test/run.sh` passed=42 failed=0; the extra edge checks
(pipelining, split writes, JWT edge cases) 24/24. After 3,000 churned connections (a third cut mid-request, a
third closed before reading the answer) the open descriptor count is back to its idle value; with 300 idle
sockets open, health and feed answer 200, and the count drops back when they close. Keep-alive and pipelined
requests answer every request. With 10 clients pipelining `GET /feed` and never reading, `/health` still
answers in under 1 ms and RSS stays at about 36 MB (each such client holds up to one read's worth of answers,
about 2 MB of `/feed`: 200 of them took RSS to 440 MB). Out of descriptors, the server stops waking up for the
listening socket (one log line instead of one per attempt) and accepts again as soon as a connection closes.

## Not handled

The spec does not ask for them: `Transfer-Encoding: chunked`, `Expect: 100-continue`, `Connection: close`
(the client closes), bodies on `GET`, and lone UTF-16 surrogates in request JSON (rejected as malformed).
A request line that does not parse, or an unfinished request over 64 KB, closes that connection.
