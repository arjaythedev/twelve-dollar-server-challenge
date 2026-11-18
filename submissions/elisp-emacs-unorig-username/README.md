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
| Front | **Behind Nginx** (`bench/nginx.conf`), listening on `127.0.0.1:3000` |

## Why Nginx

Emacs waits on its sockets with `select`, so one process can watch at most 1024 descriptors. Thousands
of direct keep-alive connections are out of reach; a few dozen upstream connections from Nginx are easy.

## How it works

- **One process, one thread, one event loop.** `accept-process-output` is the loop. Each connection is
  an Emacs network process whose filter receives raw bytes as unibyte strings.
- **HTTP/1.1 keep-alive.** The filter finds `\r\n\r\n` with `string-search`, reads `Content-Length`,
  and answers every complete request in the chunk (pipelined requests work). A partial request is kept
  on the process plist until the rest arrives. Connections are never closed by the server.
- **Routing without regexps on the read path.** The request line is compared with `string=` and
  `string-search`; regexps are only used for the two headers and the two `/posts/:id` routes.
- **Every request reads SQLite.** Nothing is cached between requests. Reads are one statement each, and
  the post object is built by SQLite's `json_object` in the exact key order, so Emacs gets one string
  back and does not cons 20 rows of Lisp data per feed. That alone was worth about 30% throughput
  over `json-serialize` on row lists. Created posts are serialized by `json-serialize`.
- **Every token is verified.** Two `secure-hash` calls per request (inner and outer HMAC pass) over
  pads derived once from `JWT_SECRET`, then the `alg`, `exp`, `nbf`, `sub` and `username` checks.
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
  accepting at 1000 connections and resumes when one closes, so a spike queues in the kernel backlog
  and never spins on a failing `accept`.
- Native compilation (`libgccjit`) works on this build but measured the same as byte code, because
  the time is spent in C primitives. So `build.sh` just byte-compiles.

## Numbers (local, rough)

Measured in a 1-CPU, 2 GB container (x86_64 under emulation on a shared laptop VM), Nginx with one
worker in the same container, k6 `bench/load.js` from other containers, 60 s ramp and a 2 minute hold.
Not the benchmark box; treat them as a sketch.

| VUs | req/s | p95 | p99 | failed | CPU (container) | RSS Emacs |
|---|---|---|---|---|---|---|
| 2,000 | 166 | 1.5 ms | 2.4 ms | 0% | 14% | 76 MB |
| 5,000 | 415 | 0.6 ms | 1.4 ms | 0% | 15% | 76 MB |
| 10,000 | 832 | 0.5 ms | 1.4 ms | 0% | 20% | 76 MB |
| 20,000 | 1,664 | 3.8 ms | 9.8 ms | 0% | 41% | 76 MB |
| 40,000 | 3,328 | 4.6 ms | 15 ms | 0% | 82% | 76 MB |
| 50,000 | 4,158 | 1.6 ms | 7.6 ms | 0% | 88% | 76 MB |
| 60,000 | 4,983 | 9.0 ms | 466 ms | 0% | 99% | 78 MB |
| 70,000 | 5,516 | 3.9 s | 31 s | 12% | 100% | 80 MB |

p99 crosses 1 s somewhere between 60,000 and 70,000 users here. Emacs is not the larger half of that:
with `wrk` on 64 connections it serves about 17,000 req/s direct (about 57 microseconds of CPU per
request in the test mix) and 11,000 req/s through Nginx.

## Known limits

- At most about 1000 open connections (see above), which is why it must sit behind Nginx.
- Request bodies need `Content-Length`; chunked uploads are not parsed. A request over 1 MB drops
  the connection.
- A query string is ignored; a trailing slash makes a path unknown (404).
