# tcl-tclsh: SQLite goes home

SQLite started life in 2000 as a Tcl extension, and its own test suite is still written in Tcl. So this
entry serves the feed API from plain `tclsh8.6`, talking to the database through the binding SQLite
grew up with: `package require sqlite3`, `db eval`. No framework, no helper process. One file of Tcl.

| | |
|---|---|
| Language | Tcl 8.6 |
| Runtime | `tclsh8.6`, Ubuntu package `tcl8.6 8.6.14+dfsg-1build1` |
| HTTP | `socket -server` plus `fileevent` on non-blocking binary channels, written here |
| SQLite | The Tcl binding `libsqlite3-tcl 3.45.1-1ubuntu2` (`package require sqlite3`), in-process, on the system `libsqlite3.so.0` (3.45.1) |
| JSON | SQLite's JSON functions: `json_object` builds every response that has a post, `json_valid` / `json_each` / `json_extract` parse request bodies and JWT payloads |
| JWT | HS256 over tcllib 1.21's C SHA-256 (`sha256c`, compiled from the source tcllib ships, with critcl 3.1.18.1, by `build.sh`) |
| Front | **Direct**, listening on `0.0.0.0:80` (no Nginx) |

## Why direct, with 1024 descriptors

Tcl 8.6 waits on its channels with `select()` (the `epoll` notifier only arrived in Tcl 8.7/9.0), so one
process can watch at most 1024 descriptors, and its notifier finds a socket's handler by searching a
list of all of them, so every event costs time for each open one. So the server cannot hold one
keep-alive connection per user. It keeps up to 32 connections alive; past that
a response carries `Connection: close`, the server closes the socket after writing it, and that user
reconnects for the next request. Connections are short, so only a few dozen are open at a time. The
server still closes connections past 1000, so no descriptor above 1023 ever reaches `select()`.

The first version sat behind Nginx, which held 24,000 users on the benchmark box and failed above that
on POSTs that got no response (0.97% errors at 24,000, 1.35% at 25,000), with CPU to spare. Every entry
behind Nginx that reached 24,000 stopped there with the same errors, which points at Nginx's 16,384
`worker_connections` for 24,000 keep-alive users rather than at the app. Going direct also drops
Nginx's share of the CPU.

## How it works

- **One process, one thread, one event loop** (`vwait forever`). Each connection is a Tcl channel in
  `-translation binary -buffering full` mode; its readable handler reads what has arrived, answers every
  complete request in it (pipelined and split requests both work) and flushes once.
- **Bytes all the way.** Because the channels are binary, a Tcl string here is one char per byte. Request
  text goes into SQLite as a blob (`@var`) and is `CAST` to `TEXT` in SQL; responses come back from SQLite
  as `CAST(... AS BLOB)`, which the binding hands over as a Tcl byte array that `puts` writes untouched.
  UTF-8 (emoji included) is never decoded or re-encoded by Tcl.
- **Keep-alive within a budget.** An idle connection is never closed by the server; a response sent
  while 32 or more connections are open is the last one on its connection (see above). A request with
  `Connection: close` also gets that header back.
- **SQLite writes the JSON.** A post, the feed (20 posts with their live like counts) and a created post are
  each one statement that returns the finished body, so Tcl touches one value per request, not 100 columns.
  Statements are prepared once and reused from the binding's statement cache (`db cache size 32`).
- **Strict JSON for free.** `json_valid()` is strict RFC 8259 (no trailing garbage, bad escapes or JSON5),
  `json_each` gives the last `body` key when a key is repeated, and `trim()`/`length()` do the trimming and
  the 500-character check in SQL.
- **Every token is verified.** HMAC-SHA256 with two hash contexts that have already absorbed the key
  pads (built once at start), two C calls per token, then keyed hashes of the expected and given signatures are compared. The
  header must say `HS256`; `exp` is required and must be a number in the future, `nbf` if present must be
  a number in the past, `sub` a positive integer string, `username` a string. Nothing is remembered.
- **Writes.** WAL, `synchronous=NORMAL`, autocommit: `db eval` returns after the `INSERT` has committed,
  and only then is the 201 written. A like is one statement
  (`INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`); a second query runs only when it
  changed nothing, to tell "already liked" from "no such post". No group commit: writes are about 8% of
  the load test, almost never two in one event-loop turn, and a WAL commit without fsync is cheap.
- **Ids.** Leading zeros are fine, a query string is ignored, an id too long for a row id is a 404, and
  `/posts/` is an unknown path.

Profile (in-process, `micro.tcl`, emulated x86_64): the feed query is about 28 us and the whole feed
handler about 30 us, so SQLite does the work; a single post is 3.6 us of query and 9 us of handler; a
token check is about 22 us, most of it the payload's JSON query. The rest of a request's cost is the
channel and `select()` round trip.

## Numbers (local, rough)

Measured in a container on one core of a shared laptop VM (x86_64 under emulation), throttled to
1 ms in every 1.7 ms of that core so that the first version behind Nginx used about the same CPU share
at 24,000 users as on the benchmark box (60 to 65% here, 59% there). k6 `bench/load.js` from three
other cores, 60 s ramp, 2 minute hold, two runs per row, run alternately with the first version behind
Nginx. Not the benchmark box; treat them as a sketch.

| VUs | req/s | p95 | p99 | failed | CPU (of the throttled core) | CPU per request |
|---|---|---|---|---|---|---|
| 24,000 | 1,997 | 0.5 to 1.8 ms | 8.4 to 22 ms | 0% | 49 to 57% | 116 to 137 us |
| 30,000 | 2,494 | 0.6 to 2.4 ms | 19 to 37 ms | 0% | 56 to 66% | 106 to 126 us |
| 36,000 | 2,993 | 6.6 to 6.7 ms | 56 ms | 0% | 73 to 74% | 116 to 118 us |
| 42,000 | 3,493 | 20 ms | 79 to 97 ms | 0% | 81 to 82% | 111 to 112 us |
| 45,000 | 3,742 | 3.4 to 17 ms | 48 to 92 ms | 0% | 70 to 78% | 88 to 103 us |
| 48,000 | 3,996 | 6.2 to 7.4 ms | 56 to 63 ms | 0% | 72 to 76% | 86 to 89 us |
| 51,000 | 4,243 | 9.5 to 10 ms | 64 to 65 ms | 0% | 77 to 78% | 86 us |
| 60,000 (one run) | 4,989 | 48 ms | 108 ms | 0% | 88% | 84 us |

The first version behind Nginx in the same runs: every run from 24,000 to 48,000 passed (60 to 98% of the throttled
core, 111 to 155 us per request, Tcl plus Nginx) except two that fell inside a VM-wide stall that also hit
other benchmarks running at the time; single runs at 54,000 and 60,000 passed at 98 and 101%, and 66,000
failed (2.8% errors). So here both versions top out between 60,000 and 66,000. Nginx logged "worker_connections are not enough" about 165 to 190
times per run from 24,000 up, but here that cost 0 to 9 POSTs per run, not the 1% the benchmark box
sees, so this setup does not reproduce the 24,000 limit; what it does show is the CPU: CPU per request
is 10 to 25% lower direct at the same load, and the VM's quiet periods lower every arm's number (the rows above span
both).

`test/run.sh`: 42 passed, 0 failed.

## Known limits

- At most 32 users keep their connection between requests; everyone else reconnects for each request
  (see above). A pipelined request after a `Connection: close` response is not answered. At most 1000
  connections are open at once; more are closed on accept.
- Request bodies need `Content-Length`; chunked uploads are not parsed. A request over 1 MB drops the
  connection.
- `Expect: 100-continue` is not answered; k6 does not send it.
