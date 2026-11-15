# tcl-tclsh: SQLite goes home

SQLite started life in 2000 as a Tcl extension, and its own test suite is still written in Tcl. So this
entry serves the feed API from plain `tclsh`, talking to the database through the binding SQLite
grew up with: `package require sqlite3`, `db eval`. No framework, no helper process. One file of Tcl.

| | |
|---|---|
| Language | Tcl 8.7 |
| Runtime | `tclsh8.7` built by `build.sh` from the Tcl 8.7a5 release tarball (SHA-256 checked), with its epoll notifier and one small patch (below) |
| HTTP | `socket -server` plus `fileevent` on non-blocking binary channels, written here |
| SQLite | The Tcl binding `libsqlite3-tcl 3.45.1-1ubuntu2` (`package require sqlite3`, a stubs extension that 8.7 loads), in-process, on the system `libsqlite3.so.0` (3.45.1) |
| JSON | SQLite's JSON functions: `json_object` builds every response that has a post, `json_valid` / `json_each` / `json_extract` parse request bodies and JWT payloads |
| JWT | HS256 over tcllib 1.21's C SHA-256 (`sha256c`, compiled from the source tcllib ships, with critcl 3.1.18.1, by `build.sh`) |
| Front | **Direct**, listening on `0.0.0.0:80` (no Nginx); every client keeps its keep-alive connection |

## Why direct, and why Tcl 8.7

The first version ran Ubuntu's Tcl 8.6 behind Nginx, because 8.6 waits on its channels with `select()`
and so can watch at most 1024 descriptors. On the benchmark box it held 24,000 users and failed above
that on POSTs that got no response (0.97% errors at 24,000, 1.35% at 25,000) with CPU to spare. Every
entry behind Nginx that reached 24,000 stopped there with the same errors, which points at Nginx's
16,384 `worker_connections` for 24,000 keep-alive users rather than at the app.

Serving direct means holding one keep-alive connection per user. Tcl 8.7 has an `epoll(7)` notifier
on Linux, so `build.sh` builds Tcl 8.7a5 (an 8.7 alpha release) from its tarball and the server runs
on it. Two more changes make that hold up:

- **Find a socket's handler by its fd.** 8.7a5 keeps its file handlers in a list and searches it on
  every event, on every change of what a channel waits for (several per request) and on every removal,
  including the removal Tcl asks for on a fresh socket before it has a handler at all. Each of those
  walked every open socket. With 16,000 idle sockets one request on another took 630 us instead of 20,
  and accepting a connection cost more with every connection already open, so with 30,000 and 36,000
  users the server stalled for seconds while they were still connecting. `tcl-epoll-fd-table.patch` (applied by
  `build.sh`; it changes only how the epoll notifier finds handlers) replaces that list with a table
  indexed by fd. After it, a request takes the same 20 us with 0 or 16,000 idle sockets, and accepting
  no longer depends on how many are open.
- **4 KB channel buffers.** Each channel keeps its buffers between requests. With the 256 KB buffers
  of the first version, 24,000 open connections took 930 MB of RSS; with Tcl's default 4 KB it is
  about 270 MB, at the same CPU per request.

`start.sh` raises the process's own descriptor limit (`ulimit -n 65536`, or the hard limit if lower)
and passes it to the server, which closes new connections 64 short of it so `accept` never fails.

## How it works

- **One process, one thread, one event loop** (`vwait forever`). Each connection is a Tcl channel in
  `-translation binary -buffering full` mode; its readable handler reads what has arrived, answers every
  complete request in it (pipelined and split requests both work) and flushes once.
- **Bytes all the way.** Because the channels are binary, a Tcl string here is one char per byte. Request
  text goes into SQLite as a blob (`@var`) and is `CAST` to `TEXT` in SQL; responses come back from SQLite
  as `CAST(... AS BLOB)`, which the binding hands over as a Tcl byte array that `puts` writes untouched.
  UTF-8 (emoji included) is never decoded or re-encoded by Tcl.
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
channel and event-loop round trip.

## Numbers (local, rough)

Measured in a container on one core of a shared laptop VM (x86_64 under emulation), throttled to
1 ms in every 1.7 ms of that core so that the first version behind Nginx used about the same CPU share
at 24,000 users as on the benchmark box (60 to 65% here, 59% there). k6 `bench/load.js` from three
other cores, 60 s ramp, 2 minute hold, two runs per row unless noted, run alternately with the first
version behind Nginx. Not the benchmark box; treat them as a sketch.

| VUs | req/s | p95 | p99 | failed | CPU (of the throttled core) | CPU per request | RSS tclsh |
|---|---|---|---|---|---|---|---|
| 24,000 | 1,995 | 1.2 to 2.6 ms | 8.3 to 22 ms | 0% | 52% | 124 to 128 us | 273 MB |
| 30,000 | 2,496 | 1.2 to 1.6 ms | 12 to 17 ms | 0% | 59 to 61% | 113 to 117 us | 331 MB |
| 36,000 | 2,993 | 5.6 ms | 53 ms | 0% | 71% | 114 us | 390 MB |
| 42,000 | 3,491 | 20 to 87 ms | 72 to 216 ms | 0% | 79 to 86% | 109 to 120 us | 449 MB |
| 48,000 | 3,991 | 17 to 39 ms | 73 to 97 ms | 0% | 77 to 80% | 91 to 97 us | 510 MB |
| 54,000 | 4,490 | 73 to 78 ms | 130 to 131 ms | 0% | 86 to 87% | 92 to 93 us | 570 MB |
| 60,000 | 4,984 | 146 to 152 ms | 197 to 209 ms | 0% | 95% | 92 us | 629 MB |
| 60,000, 5 minute hold | 5,589 | 159 ms | 201 ms | 0% | 95% | 89 us | 644 MB |
| 66,000 (one run) | 5,467 | 233 ms | 1,051 ms | 0.8% | 100% | 89 us | 682 MB |

66,000 fails: the CPU is full, and 66,000 sockets are past the descriptor limit `start.sh` asks for.
A 36,000 run that failed (p99 2.1 s at 73% CPU, no errors) is left out: other benchmarks on the VM
stalled for the same seconds. The first version behind Nginx, in the same runs, passed every run from
24,000 to 48,000 except two inside such stalls, at 60 to 98% CPU and 111 to 155 us per request (Tcl plus
Nginx); single runs at 54,000 and 60,000 passed at 98 and 101% CPU (p99 66 and 302 ms), and 66,000 failed
(2.8% errors). So here both versions top out between 60,000 and 66,000 users. Its Nginx logged
"worker_connections are not enough" about 165 to 190 times per run but lost only 0 to 9 POSTs, so this
setup does not reproduce the 24,000 limit of the benchmark box. What it does show is that the direct
server uses 10 to 20% less CPU per request at the same load, with a lower tail near the limit.

`test/run.sh`: 42 passed, 0 failed.

## Known limits

- At most the descriptor limit minus 64 open connections (65,472 with the limit `start.sh` asks for).
- Tcl 8.7a5 is an alpha release; the server uses its sockets, channels and event loop only.
- Request bodies need `Content-Length`; chunked uploads are not parsed. A request over 1 MB drops the
  connection.
- `Expect: 100-continue` is not answered; k6 does not send it.
