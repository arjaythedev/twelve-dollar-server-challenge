# C on h2o

| | |
|---|---|
| Language | C (GNU C11), compiled by zig 0.15.2 (`zig cc`, clang), `ReleaseFast`, native CPU |
| Framework | [h2o](https://github.com/h2o/h2o) as a library (`libh2o-evloop`, epoll) |
| Libraries | SQLite 3.53.2 (amalgamation), libsodium (HMAC-SHA256, base64url), [arnm](https://github.com/gradido/arnm) (arenas and arena pools, JSON reader and writer based on yyjson), LibreSSL 4.0.0 and zlib 1.3.1 (h2o links them). All pinned in `build.zig.zon` with their hashes and built from source |
| **Nginx or direct** | **Direct**: the server listens on `0.0.0.0:80` |

The server is written for this benchmark, but on a stack that is being built for another
project: the C fast path of [gradido-unicorn](https://github.com/gradido/gradido-unicorn), which
is still in development. It uses the same HTTP library, thread model, JSON and crypto libraries,
and build system. That stack is specialised for its own job and was not chosen for this one, so
the question the submission answers is how far it goes on the box, not how far the box goes.

## Run

```bash
sudo bash install.sh   # zig 0.15.2 (checksummed), libc6-dev, curl
bash build.sh          # fetches the pinned sources and builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The server reads `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`. `SERVER_THREADS` overrides the
number of event loops (default: one per core).

The first build compiles h2o, LibreSSL, SQLite, arnm and libsodium, which takes several minutes on one
core. Starting takes well under a second.

## Results

Not measured on a droplet. On a development machine (Ryzen 7 5700G, kernel 6.1), with the
load on other cores over loopback, using the load generator of the `c-iouring-raoz` submission
(`tests/loadgen.c`, the user loop of `bench/load.js`).

Direct, the server pinned to one core, both servers measured one after the other:

| Test | This server | `c-iouring-raoz` |
|---|---|---|
| 60,000 users (6,350 requests/s): CPU | 36% of the core | 14% of the core |
| 60,000 users: errors, p95, p99 | 0, 0.5 ms, 2.0 ms | 0, 7.0 ms, 34 ms |
| 60,000 users: resident memory | 129 MB | 39 MB |
| No think time, 256 connections | about 48,000 requests/s | about 68,000 requests/s |

At full load this stack needs about 1.4 times the CPU per request as `c-iouring-raoz`; with think time the gap is
larger. More than half of this server's CPU time at 60,000 users is spent in the kernel (one
`read` and one `sendmsg` per request, and SQLite's `pread`s), about a quarter in SQLite's reads.
Letting the loop rest 1-2 ms between passes, so that one `epoll_wait` meets the requests of that
time, was tried and changed nothing measurable, so it is not in. The limit on the box is the CPU
or the 65,535 descriptors, whichever comes first.

The same binary also runs behind Nginx (`HOST=127.0.0.1 PORT=3000`). With Nginx 1.22 and
`bench/nginx.conf` (one worker, `worker_connections 16384`), Nginx and the server pinned to the
same core:

| Test | Server | Nginx | Together |
|---|---|---|---|
| 15,000 users (1,590 requests/s): CPU | 10% of the core | 9% | 20% |
| 15,000 users: errors, p95, p99 | 0, 0.5 ms, 0.9 ms | | |
| 16,300 users: CPU | 11% of the core | 12% | 23% |
| 16,300 users: errors | 4 of 215,570 requests | | |

At 15,000 users Nginx talks to the server over 3 to 7 connections and reuses them. At 16,300
Nginx runs out of `worker_connections` and starts closing idle client connections (its log says
so 92 times), and four requests fail. Behind Nginx the number of users is bounded by Nginx; the
server is far from its own limit there.

## Design

- **One event loop per core** (h2o's thread-per-loop model), each with its own listening socket
  under `SO_REUSEPORT`. On the one-vCPU box that is one loop.
- **Reads run on the loop thread**, on that loop's own read-only SQLite connection with prepared
  statements. A read is a few B-tree lookups in SQLite's page cache and does not wait.
- **Writes go to one writer thread** that owns the only write connection and is fed through a
  queue. The request is parked on its loop and answered when its transaction has committed.
- **Group commit.** The writer takes everything that queued up while the previous transaction was
  committing and writes it in one transaction. No response is sent before the commit; if the
  commit fails, every request of that transaction gets a 500.
- **The feed uses three range scans** instead of twenty correlated subqueries: the 20 newest ids
  from the `created_at` index, one scan of `likes_post_id_idx` over their id range, one rowid
  range scan of `posts` joined to `users`. They run in one read transaction. If the ids are far
  apart, the reference query answers instead, inspired by `c-iouring-raoz`.
- **JWT.** HS256 through libsodium. The HMAC state after the key is derived once at startup;
  the signature of every token is computed and compared on every request. The claims are read
  with arnm's table reader, with the table in the order the tokens carry their members.
- **JSON.** arnm in both directions (yyjson underneath): its table reader for request bodies and
  token claims, its writer for every response that carries data. The writer keeps the order the
  members are added in, which is the order the spec asks for. Only the constant error bodies are
  literals.
- **Memory.** Each loop has one JSON writer, reused for every response, with an memory arena of its own
  for the document being built. The rendered text goes into a separate arena that is lent from a
  graded arena pool for that one response and returned when h2o disposes the request. So the
  text is sent from where it was rendered, without a copy, and the writer is free for the next
  response while this one is still on its way out. A request body is parsed into an arena from
  the same pool, which is the one copy made of it: the parked write keeps that arena, the writer
  thread reads the post's text from it, and it goes back when the write has been answered.
  Everything else per request comes from h2o's request pool; parked writes use a fixed table of
  slots per loop.
- **Arena sizes.** The writer is told how many values a document will hold, so its pool opens
  once at that size, and every string is borrowed, not copied. A lent arena is sized for the
  ordinary case: a text whose strings need no escaping, a body with one value in six bytes. When
  the serializer or the parser reports that it ran out of memory, the work is done again in an
  arena sized for the worst case (six bytes per byte of text, one value in two bytes of input).
- **Connections.** Each user holds a keep-alive connection, which is a file descriptor, so
  `LimitNOFILE=65535` caps this server at about 65,000 users. When the descriptors run out, the
  server stops accepting for a moment instead of spinning. The idle timeout is 120 s (h2o's
  defaults of 10 s and 5 s would close the connections of users who are reading); that is also
  above the 65 s the spec asks for behind Nginx.

## Rules

- No data is cached between requests. Each response comes from SQLite queries for that request,
  and every token's signature is verified.
- Each write is committed (WAL, `synchronous=NORMAL`) before its response is sent.
- The schema is not changed. `PRAGMA foreign_keys=ON` makes the `REFERENCES` clauses active.

## License

MIT, under the [license](../../LICENSE) of the repository.
