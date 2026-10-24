# Trux HTTP

[Trux](https://github.com/thea1lab/trux) is an [A1 Lab](https://github.com/thea1lab) language. Ordinary code compiles to C. GPU kernels compile to CUDA. One program can contain both, and the compiler builds them together. This feed server does not launch kernels. It is the CPU HTTP path: one native binary that serves the API and talks to SQLite in-process.

## How this server runs

- **One worker.** The box has one vCPU. SQLite is opened once for that thread. More workers would spend the core on switches and on lock waits. The queries are primary-key and index lookups.
- **`io_uring`.** That worker accepts and reads with multishot operations and a kernel buffer ring. A connection stays open between requests. There is no thread per connection.
- **Direct, not Nginx.** On one core, a proxy takes time away from the server. The process listens on `HOST` and `PORT` itself and speaks HTTP/1.1 with keep-alive.
- **SQLite, no cache.** The schema is unchanged. The connection uses WAL and `synchronous=NORMAL`, a 5-second busy timeout for write bursts, a 32 MiB page cache, and a 256 MiB mmap limit. Each request reads or writes the database. Responses and JWT checks are not stored for the next request. A write commits before its response is sent.
- **JWT on every authenticated request.** libjwt checks HS256, the signature, `exp`, a positive `sub`, and a username. A bad token is 401.
- **Likes.** One statement: `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. If that changes no row, a second query tells a repeat like from a missing post.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

`install.sh` builds [Trux `653ea863797985a76056dfe4edd40ae4c0129777`](https://github.com/thea1lab/trux/commit/653ea863797985a76056dfe4edd40ae4c0129777), Go 1.26.3, and static libjwt 3.6.1. liburing, jansson, and OpenSSL come from Ubuntu 24.04. SQLite is the copy linked into that Trux runtime.

`test/run.sh submissions/trux-http-claudioscheer` is the check CI runs after `install.sh`.

## License

MIT, under the repo's [license](../../LICENSE).
