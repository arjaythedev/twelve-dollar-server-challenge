# octave-sockets: the web server is GNU Octave

The feed API is answered by GNU Octave, the free MATLAB clone. `start.sh` runs `octave-cli server.m`
and nothing else: no Nginx, no second process. The request handling is an Octave script; a small
C++ extension (an `.oct` file built with `mkoctfile`) gives it what Octave does not have: a way to wait
on thousands of sockets and a SQLite connection.

| | |
|---|---|
| Language | GNU Octave (the MATLAB language), about 270 lines in `server.m` |
| Runtime | GNU Octave 8.4.0, Ubuntu package `octave=8.4.0-1build5`, run as `octave-cli` |
| Sockets | `ox.oct`, our own extension (`ox.cc`, about 230 lines of C++): epoll, accept, recv, send |
| SQLite | the system SQLite 3.45.1 (`libsqlite3-dev`), linked into `ox.oct`, so it runs inside the Octave process |
| JSON | Octave's built-in `jsondecode` (RapidJSON) to validate request bodies and JWT parts; SQLite's `json_object` to build post objects |
| JWT | HS256 from Octave's built-in `hash("sha256", ...)` with RFC 2104 ipad/opad, base64url done as matrix arithmetic |
| Front | **Direct**: Octave listens on `HOST:PORT` itself (`0.0.0.0:80` on the box), no Nginx |

## What is in C++ and what is in Octave

Honestly, so nobody has to read the code to find out:

- **C++ (`ox.cc`)** has seven verbs and no knowledge of HTTP: `listen`, `wait` (one `epoll_wait`,
  accept new clients, return the bytes each ready socket sent), `send` (write a whole buffer),
  `close`, `open` (the database), and `q` (run one SQL statement to completion with the given
  arguments and return its first row as text, plus the change count). Prepared statements are kept
  by SQL text and reused.
- **Octave (`server.m`)** does everything else: finding the end of each request and its
  `Content-Length`, pipelined and split requests, the request line and query string, routing,
  post id validation, the `Authorization` header, splitting the JWT, base64url decoding and encoding,
  the HMAC-SHA256, the `alg` / `exp` / `nbf` / `sub` / `username` checks, JSON validation of the
  body, trimming and counting characters, every status code and error, and the HTTP response.
- **SQLite** builds the post JSON (`json_object`) in the exact key order, so the feed comes back
  from one query as one string and Octave does not loop over 20 rows.

## The joke, done properly

A numerical computing environment speaks HTTP, so where Octave does the work it does it the way
Octave does everything, with arrays:

- **base64url is linear algebra.** Three bytes are one 24-bit number: `[65536 256 1] * reshape(b, 3, [])`.
  The four base-64 digits of every group come out of one `floor`/`mod` expression over the whole row
  vector, and indexing the alphabet with that matrix is the encoding. Decoding is the same backwards
  with `[262144 4096 64 1]`.
- **Hex to bytes** for the HMAC is `d(1:2:end) * 16 + d(2:2:end)` on the digest's character codes.
- **Counting characters** of a UTF-8 body is `sum(bitand(double(b), 192) != 128)`: every byte that
  is not a continuation byte.
- The HMAC pads are `bitxor(double(key), 54)` and `bitxor(double(key), 92)`.

## How it works

- **One process, one thread.** The main loop calls `ox("wait")`, gets a vector of descriptors and a
  cell array of byte strings, and answers every complete request in each. A partial request waits in
  a cell array indexed by descriptor. All answers for one socket go out in one `send`.
- **Sockets.** Every `recv` and `send` uses `MSG_DONTWAIT`, one `recv` of up to 64 KB per wake-up. What a
  client's socket does not take at once waits in a per-client queue and goes out on `EPOLLOUT`; while it
  waits, that client is not read, so a client that stops reading holds up nobody else and cannot make
  the server queue more. A client that hangs up is still answered for what it sent before the EOF, then
  closed.
- **Every request reads SQLite.** Nothing is cached between requests; every token gets a fresh
  HMAC (two `hash` calls). Ids are passed to SQLite as text and compared as `CAST(? AS INTEGER)`, so
  ids of any length work (an id beyond 2^63 saturates and is not found).
- **Writes.** WAL, `synchronous=NORMAL`, autocommit. `ox("q")` steps each statement until
  `SQLITE_DONE`, which is when the commit happens, so a 201 is only built after its `COMMIT`.
  A post is one `INSERT ... RETURNING json_object(...)`; a like is one
  `INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`, and only when it changes
  nothing does a second query tell "already liked" from "no such post".
- **Strict JSON.** `jsondecode` rejects trailing data, bad escapes and the rest. Octave decodes a
  JSON array of one object into a struct too, so the body must also start with `{`.
- **Errors.** Anything that throws inside a request becomes a 500 for that request; the loop goes on.

## Why direct, not Nginx

The waiting is done by epoll in `ox.cc`, so the number of connections is not a problem for the loop.
Locally, Octave alone direct and Octave behind `bench/nginx.conf` saturated at about the same
request rate, and Nginx costs a share of the one CPU (17% of it in the run below), so direct it is.

## Numbers (local, rough)

Measured on a laptop, not on the droplet: the server in a 1-CPU, 2 GB, `nofile=65535` x86_64 container
(emulated on an ARM host, a VM other jobs were using), k6 `bench/load.js` in separate containers.
60 s ramp and a 90 s hold, shorter than the scoring hold. Local runs here have overstated the droplet by
1.3x to 2.5x for other entries, so read these as an upper bound.

| Mode | k6 users | req/s | p95 | p99 | failed | CPU | Octave RSS |
|---|---|---|---|---|---|---|---|
| direct | 20,000 | 1,590 | 4.6 ms | 79 ms | 0% | 70-82% | 67 MB |
| direct | 25,000 | 1,980 | 301 ms | 619 ms | 0% | 82-92% | 66 MB |
| Nginx | 30,000 | 2,270 | 0.98 s | 17 s | 3.9% | 100% (Nginx 17%) | 62 MB |
| direct | 40,000 | 1,440 | 31 s | 42+ s | 0.6% | 97% | 35 MB |

Without think time (wrk, 64 connections, 100 feed : 100 post : 15 like : 2 create): about 4,800 req/s,
0 errors.

`test/run.sh`: 42 passed, 0 failed.

## Known limits

- Request bodies need `Content-Length`. A request with `Transfer-Encoding` gets
  411 `{"error":"length required"}` and the connection is closed. A buffered request over 1 MB drops
  the connection.
- A trailing slash makes a path unknown (404). A query string is ignored.
- `Connection: close` is left to the client.
- Past 65,535 open descriptors `accept` fails and the listener stays readable, so the loop spins on it;
  Octave is out of CPU long before that many users.
