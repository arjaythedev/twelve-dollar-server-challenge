# C on io_uring

| | |
|---|---|
| Language | C (GNU C11), compiled by zig 0.15.2 (`zig cc`, clang), `ReleaseFast`, native CPU |
| Framework | None. The HTTP/1.1 server is an io_uring loop written on the system calls (`src/uring.h`), without liburing |
| Libraries | SQLite 3.53.2 (amalgamation), libsodium (HMAC-SHA256, base64url), [arnm](https://github.com/gradido/arnm) (arenas and arena pools, JSON reader and writer based on yyjson), [picohttpparser](https://github.com/h2o/picohttpparser) (the request parser of h2o). All pinned in `build.zig.zon` with their hashes and built from source |
| **Nginx or direct** | **Direct**: the server listens on `0.0.0.0:80` |
| Kernel | Linux 6.1 or later |

This is [`c-h2o-einhornimmond`](../c-h2o-einhornimmond) with its HTTP side replaced. The database
code, the token check, the JSON reader and writer, the writer thread and its group commit are the
same files; h2o's epoll loop, and with it LibreSSL and zlib, are gone, and an io_uring loop
stands where they were.

## Run

```bash
sudo bash install.sh   # zig 0.15.2 (checksummed), libc6-dev, curl
bash build.sh          # fetches the pinned sources and builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The server reads `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.

The first build compiles SQLite, libsodium and arnm, which takes a few minutes on one core.
Starting takes well under a second.

## Results

Not measured on a droplet. On a development machine (Ryzen 7 5700G, kernel 6.1): the server in
a container with 2 GB and `LimitNOFILE=65535`, pinned to one core, the load on other cores over
loopback from the load generator of the `c-iouring-raoz` submission (`tests/loadgen.c`, the user
loop of `bench/load.js`). All three servers measured the same way, one after the other:

| 60,000 users (6,350 requests/s) | This server | `c-h2o-einhornimmond` | `c-iouring-raoz` |
|---|---|---|---|
| CPU | 19% of the core | 36% | 14% |
| Errors | 0 | 0 | 0 |
| p95, p99 | 2.3 ms, 2.9 ms | 0.5 ms, 1.0 ms | 4.9 ms, 22 ms |
| Resident memory | 51 MB | 131 MB | 38 MB |

The first column is a hold of five minutes, the others of two. With 70,000 users the table of
65,535 connections is full: those inside it are served as before (p99 3 ms), the others wait in
the backlog.

The latency of this server is its own doing: under load the loop waits up to 1 ms for requests
to collect. Without that wait the p95 is 0.2 ms and the CPU 20%; with 2 ms or 4 ms the CPU was
not lower on this kernel (21-22%), so 1 ms it is. Memory-mapped reads (`PRAGMA mmap_size`) on the
read connection were tried as well and cost more (26%), so they are not in.

## Design

- **One io_uring loop on one thread**, which is what the one-vCPU box has room for. The ring is
  set up with `SINGLE_ISSUER` and `DEFER_TASKRUN`: completions are handed over when the loop
  asks for them, not in the middle of a request.
- **One call into the kernel per pass.** It hands over every response of the pass before and
  brings back every request that arrived since. A request costs no system call of its own: no
  `epoll_wait`, `read` and `sendmsg` for each one.
- **Accept and receive are multishot.** One accept stays armed for the listener and one receive
  for every connection. A receive borrows its buffer from a ring of 4,096 provided buffers
  when data arrives, so an idle connection holds no buffer: 40 bytes of the server's memory and
  the kernel's socket.
- **Requests are served from the buffer they arrived in.** picohttpparser reads it in place and
  the buffer goes back to the kernel when the handler returns. Only a request that arrives in
  pieces, or ahead of the answer to the one before, is copied into memory of the connection.
- **A request body is parsed where it arrived.** arnm's reader parses in place: it unescapes the
  strings into the bytes of the receive buffer, and the post's text is read from there by the
  writer thread, so it is not copied between the socket and SQLite. The reader wants four bytes
  of room behind its input, so the kernel is offered every buffer four bytes short. A parked
  write keeps its buffer and gives it back with its answer. A body that arrived in pieces, or
  with the next request behind it, is copied once and parsed in the copy.
- **A response leaves in one piece**, head and text in one `send`, from an arena lent from a
  graded pool that gets it back when the send has completed. The constant error responses are
  put together once and sent from where they lie.
- **Connections are direct descriptors.** They live in the ring's own table, not in the process's
  descriptor table. The table may be as large as `LimitNOFILE` and no larger, so the server holds
  65,535 connections. When it is full the accept rests until a connection has closed.
- **Reads run on the loop thread**, on its own read-only SQLite connection with prepared
  statements. A read is a few B-tree lookups in SQLite's page cache and does not wait.
- **Writes go to one writer thread** that owns the only write connection and is fed through a
  queue. The connection is parked and answered when its transaction has committed. So the loop
  never waits for the disk: a checkpoint's `fsync` delays the writes behind it and no read.
- **Group commit.** The writer takes everything that queued up while the previous transaction was
  committing and writes it in one transaction. No response is sent before the commit; if the
  commit fails, every request of that transaction gets a 500. The loop is woken through an
  eventfd it reads in the ring.
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
- **Clients that do not behave.** Requests on one connection are answered in order, one at a
  time. A connection with more than 36 KiB waiting is closed, as is one that is idle for 120 s,
  and one whose request or response makes no progress for 30 s. A descriptor is closed only when
  nothing of its connection is in the ring or with the writer any more.

## Rules

- No data is cached between requests. Each response comes from SQLite queries for that request,
  and every token's signature is verified.
- Each write is committed (WAL, `synchronous=NORMAL`) before its response is sent.
- The schema is not changed. `PRAGMA foreign_keys=ON` makes the `REFERENCES` clauses active.

## License

MIT, under the [license](../../LICENSE) of the repository.
