# Saad: web + dbms.sqlite, epoll

The feed API in [Saad](https://github.com/jakobsaadbye/Saad), written with its standard library: the `web` package for
the endpoints, `dbms.sqlite` for the database, `jwt` for the tokens. No framework or library besides SQLite.

| | |
|---|---|
| Language | Saad, built from source at a pinned commit by `build.sh` (its compiler is written in C) |
| Compiler backend | LLVM 18 (Ubuntu 24.04's `llvm-18-dev`), `-O3` (`saad build --llvm -O3`) |
| Framework | Saad's standard library: `web` (endpoints), `net.http` (the server), `jwt`, `json` |
| SQLite | Ubuntu 24.04's `libsqlite3` (3.45.1), through `dbms.sqlite` |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## The code

`main.sd` is the whole program: five endpoints, e.g

```odin
@web.endpoint(.get, path: "/posts/{id}")
getPost :: (session: *Session, id: string) -> PostReply {
    post, found := db().queryOne<Post>(PostSql, postId(id));
    if !found { throw errors.make("post not found", kind: web.NotFound); }
    return PostReply{post};
}
```

* A result is written as JSON by `web`, with snake_case names (`createdAt` is `created_at`), so `Post` is both the row
  a query reads and the object of the response.
* `POST /posts` and `POST /posts/{id}/like` have `authorize: true`: the token is checked (HS256 only, the signature in
  constant time, `exp` required) before the id or the body is read, so a request without a token is a 401 first.
* The errors of the spec are thrown with kinds, e.g `web.BadRequest`, and the app gives the errors of `web` itself the
  messages of the spec, e.g `app.setMessage(web.MalformedJson, "malformed JSON body")`. Every error body is
  `{"error":"..."}`, and a 500 is `internal server error`.

## What makes it fast

* **An event loop on epoll.** `net.http`'s server is a thread with a `Poller` (epoll) of its own connections, on
  non-blocking sockets. It reads what has come, answers the requests that are whole, and sends each response with one
  `send`. An idle keep-alive connection costs no thread and keeps no buffer, only a struct of about 100 bytes, so a
  process holds tens of thousands of them in a few MB.
* **The responses of a round are written after it.** The requests that one wait of epoll gives are all answered first,
  then all their responses are written. A `send` runs the network stack of the kernel, which takes the processor's
  caches from SQLite and the rest of the program, so answering them back to back makes each faster (a `/feed` query
  takes about 22 µs on warm caches and about 32 µs right after a 5 KB `send`).
* **Processes for the connections.** Every k6 user holds a connection, and `LimitNOFILE=65535` caps a process, so
  `start.sh` runs 4 processes on the port with `SO_REUSEPORT` (Linux shares the connections between them), room for
  about 260,000 connections. Each raises its soft file limit to the hard one. On one core, 4 processes answer about 11%
  fewer requests than 1, nearly all of it from the writes: when another process has committed, SQLite drops its page
  cache and maps the database again at the next read, and reads the pages that are in the log again.
* **Prepared statements, kept.** `dbms.sqlite` prepares each SQL once per connection and steps it again for the next
  request; the rows are read into a buffer that the next query uses again. Integers come from SQLite as numbers, not
  as text to parse. Prepared statements cache the plan, not the data (rule 5): every request reads its rows.
* **JSON without reflection per value.** The members of a struct, with their keys (e.g `"like_count":`), are worked out
  once per type, and strings are copied in runs of characters that need no escape, which are found 8 bytes at a time.
* **Checkpoints off the request path.** WAL with `synchronous=NORMAL` (rule 6): a 201 is sent after its `COMMIT`. A
  thread of the first process runs `PRAGMA wal_checkpoint(PASSIVE)` every 250 ms on its own connection, so no request
  waits while the log is copied into the database; past 64 MB of log a commit checkpoints by itself, as a last resort.
* **Memory.** The database is read through `mmap` (shared by the processes), with a page cache of 16 MB each. Each
  request's memory is an arena that is cleared after its response.

## Measured

On one core of a 4-core Xeon @ 2.1 GHz (the server pinned to it, the load on the others, over loopback):

* The mix of `bench/load.js` (per user: a feed, a post, 15% likes, 2% posts) with `wrk`: about 36,000 requests/s with
  1 process, 32,000 with 4 (24,600 and 22,900 with the standard library at 68b89f2). With 4 processes a
  request takes about 35 µs of the core: 14 in SQLite, 8.5 in the program, 11 in the kernel.
* `GET /feed` alone: 23,500 requests/s; `GET /posts/:id` alone: 80,000.
* `wrk` with 10,000 connections of the mix on 4 processes: 26,700 requests/s (20,800 before), 0 errors.
* k6 with `bench/load.js` and 15,000 users (as many as one machine's descriptors allow k6 here), with the build before
  the rounds: 0 errors, p95 0.6 ms, p99 2.5 ms, 27 MB for the 4 processes.

## Build and run

```bash
sudo bash install.sh     # build-essential, git, llvm-18-dev, libsqlite3-dev
bash build.sh            # clones Saad at SAAD_COMMIT, builds its compiler, then bin/server
bash start.sh            # PROCESSES=4 copies on HOST:PORT
```

`SAAD_SOURCE=/path/to/Saad bash build.sh` builds with a Saad checkout that is there instead of cloning it.
`WORKERS` gives each process more threads (1 unless given).
