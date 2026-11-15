# elisp-emacs: the web server is GNU Emacs

The whole API is one Emacs Lisp file run by `emacs --batch`. No C, no modules, no helper process:
the editor listens on the socket, parses HTTP, checks the JWTs and talks to SQLite.

| | |
|---|---|
| Language | Emacs Lisp (byte-compiled by `build.sh`) |
| Runtime | GNU Emacs 29.3, Ubuntu package `emacs-nox 1:29.3+1-1ubuntu2` |
| HTTP | `make-network-process :server` plus a process filter, written here |
| SQLite | Emacs's built-in binding (`sqlite-open`, `sqlite-select`, `sqlite-execute`), system SQLite 3.45.1 |
| JSON | Emacs's built-in `json-parse-string` / `json-serialize` (Jansson 2.14), and SQLite's `json_object` for reads |
| JWT | HS256 from `secure-hash` with RFC 2104 ipad/opad, `base64-decode-string` in base64url mode |
| Front | **Direct**, listening on `0.0.0.0:80` (no Nginx) |

## Why direct, with 1024 descriptors

Emacs waits on its sockets with `select`, so one process can watch at most 1024 descriptors, and
every wakeup costs time for each open one: with wrk on 32 connections, 900 extra idle connections
took Emacs from 55 to 227 microseconds per request. So Emacs cannot hold one keep-alive connection
per user. It keeps up to 32 connections alive; past that a response carries `Connection: close`, the
server closes the socket after writing it, and that user reconnects for the next request. The
listener pauses at 256 open connections, so a burst waits in the kernel's accept queue (backlog
4096) instead of in the `select` set.

The first version sat behind Nginx, which held 24,000 users on the benchmark box and failed above
that on POSTs that got no response. All four entries behind Nginx that reached 24,000 stopped there
with the same errors (Lua, Crystal, Go and this one, at 49 to 93% CPU), which points at Nginx's
16,384 `worker_connections` for 24,000 keep-alive users rather than at the app. Emacs also slowed down behind Nginx past that point, because Nginx opened
more and more upstream connections into the `select` set.

## How it works

- **One process, one thread, one event loop.** `accept-process-output` is the loop. Each connection is
  an Emacs network process whose filter receives raw bytes as unibyte strings.
- **HTTP/1.1 keep-alive.** The filter finds `\r\n\r\n` with `string-search`, reads `Content-Length`,
  and answers every complete request in the chunk (pipelined requests work). A partial request is kept
  on the process plist until the rest arrives. An idle connection is never closed by the server; a
  response past the 32-connection budget is the last one on its connection (see above).
- **Routing without regexps on the read path.** The request line is compared with `string=` and
  `string-search`; regexps are only used for the two headers and the two `/posts/:id` routes.
- **Every request reads SQLite.** Nothing is cached between requests. Reads are one statement each, and
  the post object is built by SQLite's `json_object` in the exact key order, so Emacs gets one string
  back and does not cons 20 rows of Lisp data per feed. That alone was worth about 30% throughput
  over `json-serialize` on row lists. Created posts are serialized by `json-serialize`.
- **Every token is verified.** Two `secure-hash` calls per request (inner and outer HMAC pass) over
  pads derived once from `JWT_SECRET`, a constant-time comparison of the signature, then the `alg`,
  `exp`, `nbf`, `sub` and `username` checks.
- **Writes.** WAL, `synchronous=NORMAL`, autocommit: each `INSERT` is its own transaction and has
  committed before the response is written. A like is one statement
  (`INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`); only when it changes
  nothing does a second query tell "already liked" from "no such post". Group commit was measured and
  left out: a batch of ten saves about 20 microseconds per write, and at this load a batch is almost
  always a single write.
- **GC.** `gc-cons-threshold` is 16 MB: frequent enough that a collection is a short pause, rare
  enough to be cheap. 256 MB gave the same throughput with twice the p99 and 260 MB more RSS.
- **Overload.** `start.sh` ignores SIGPIPE (a batch Emacs would otherwise die when a peer hangs up
  mid-response), the filter turns any write error into a dropped connection, and the listener stops
  accepting at 256 connections and resumes when one closes, so a spike queues in the kernel backlog
  and never spins on a failing `accept`.
- Native compilation (`libgccjit`) works on this build but measured the same as byte code, because
  the time is spent in C primitives. So `build.sh` just byte-compiles.

## Numbers (local, rough)

Measured in a container on one core of a shared laptop VM (x86_64 under emulation), throttled to half
of that core in 1 ms slices so that the first version behind Nginx used the same CPU share at 24,000
and 30,000 users as on the benchmark box (77 to 91% and 94 to 100% here, 76% and 96% there). k6
`bench/load.js` from other containers, 60 s ramp, 2 minute hold unless noted. Not the benchmark box;
treat them as a sketch.

| VUs | req/s | p95 | p99 | failed | CPU (of the throttled core) | CPU per request |
|---|---|---|---|---|---|---|
| 24,000 | 1,996 | 0.8 to 9.3 ms | 11 to 125 ms | 0% | 60 to 78% | 122 to 155 µs |
| 30,000 | 2,494 | 4.3 to 12 ms | 17 to 56 ms | 0% | 71 to 82% | 116 to 137 µs |
| 36,000 | 2,995 | 9.7 to 9.9 ms | 30 to 33 ms | 0% | 85 to 87% | 115 to 116 µs |
| 40,000 | 3,328 | 12 to 14 ms | 41 to 51 ms | 0% | 92% | 111 to 113 µs |
| 45,000 | 3,740 | 20 to 50 ms | 72 to 119 ms | 0% | 95 to 98% | 104 to 108 µs |
| 45,000, 5 min hold | 4,197 | 55 ms | 109 ms | 0% | 97% | 105 µs |
| 50,000 | 4,087 to 4,146 | 224 to 521 ms | 515 to 660 ms | 0% | 99 to 100% | 98 to 103 µs |

Two runs per row except the hold. The same setup with the first version behind Nginx: 24,000 passed
(155 to 180 µs per request, Emacs plus Nginx), 27,000 passed once and failed once, 30,000 failed both
times (21 to 27% errors, Nginx upstream timeouts).

## Known limits

- At most 32 users keep their connection between requests; everyone else reconnects for each request
  (see above). A pipelined request after a `Connection: close` response is not answered.
- Request bodies need `Content-Length`; chunked uploads are not parsed. A request over 1 MB drops
  the connection.
- A query string is ignored; a trailing slash makes a path unknown (404).
