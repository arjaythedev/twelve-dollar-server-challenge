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
| Front | **Behind Nginx** (`bench/nginx.conf`), listening on `127.0.0.1:3000` |

## Why Nginx

Tcl 8.6 waits on its channels with `select()` (the `epoll` notifier only arrived in Tcl 8.7/9.0), so one
process can watch at most 1024 descriptors. Thousands of direct keep-alive connections are out of
reach; the few dozen upstream connections Nginx keeps are easy. The server refuses (closes) connections
past 1000 so no descriptor above 1023 ever reaches `select()`.

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
channel and `select()` round trip.

## Numbers (local, rough)

A 1-CPU, 2 GB, no-swap cgroup holding Nginx (one worker) and `tclsh` on a shared laptop VM, x86_64
under emulation; k6 `bench/load.js` outside the cgroup, 30 s ramp and a 1 minute hold. Other benchmarks
were running on the same VM (load average 10 to 30 on 12 cores), so treat these as a sketch. Local runs
like this have overstated the benchmark box by about 1.3x to 2.5x for earlier entries.

| VUs | req/s | p95 | p99 | failed | CPU (cgroup) | Memory (cgroup, incl. page cache) | RSS tclsh |
|---|---|---|---|---|---|---|---|
| 20,000 | 1,596 | 3.2 ms | 10.7 ms | 0% | 45% | 394 MB | 70 MB |
| 30,000 | 2,390 | 1.7 ms | 5.6 ms | 0% | 60% | 549 MB | 62 MB |
| 40,000 | 3,191 | 3.2 ms | 10.3 ms | 0% | 80% | 391 MB | 73 MB |
| 50,000 | 3,987 | 2.1 ms | 9.5 ms | 0% | 85 to 90% | 575 MB | 74 MB |
| 60,000 | 4,782 | 2.6 ms | 10.5 ms | 0% | 97 to 99% | 580 MB | 79 MB |

The CPU is full at 60,000 users, so the limit here is a little above that. A first 40,000 run during a
busier moment on the VM failed (p99 2.5 s), which says how noisy this is.

Without think time (`wrk`, 64 connections, 100 feed : 100 post : 15 like : 2 create): about 21,000 req/s
direct to Tcl (42 us of CPU per request) and 14,900 req/s through Nginx.

`test/run.sh`: 42 passed, 0 failed.

## Known limits

- At most 1000 open connections (see above), which is why it sits behind Nginx.
- Request bodies need `Content-Length`; chunked uploads are not parsed. A request over 1 MB drops the
  connection.
- `Expect: 100-continue` is not answered; Nginx does not send it.
