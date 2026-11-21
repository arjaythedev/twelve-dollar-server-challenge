# C++23 + io_uring

A C++23 HTTP/1.1 server for the $12 Server Challenge. One event loop owns
connections, parses requests and runs the API. A separate thread checkpoints SQLite.

This submission serves directly, without Nginx: `HOST=0.0.0.0`, `PORT=80`.
The server reads `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT` from the environment.

## Build and run

Run these commands from this directory on Linux:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/absolute/path/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` installs build tools and OpenSSL headers. `build.sh` builds `.build/server`
from `src/`, downloading pinned source dependencies. `start.sh` runs it in the foreground.
Port 80 needs `CAP_NET_BIND_SERVICE`; use a higher port in a normal local shell.

| Component | Version |
| --- | --- |
| Language | C++23 |
| Network I/O | liburing 2.15 |
| HTTP parser | llhttp 9.4.3 |
| Database | SQLite 3.53.1, compiled in-process |
| JSON parser | yyjson 0.13.0 |
| JWT HMAC | OpenSSL 3, Ubuntu `libssl-dev` package |

## What does the work

- One event loop drives up to 16 io_uring rings, created as connections need them.
  Direct descriptors and 64 outstanding accepts keep socket bookkeeping in the rings.
  Receives are one-shot; each connection has an explicit owner and buffer lifetime.
- Busy turns wait for 128 completions or an 8 ms timeout, with shorter waits for
  receive retries. Each ring drains at most 256 completions per turn.
- Writes collected during a turn share a transaction, capped at 32 requests.
  Every success response waits for its commit. SQLite uses WAL,
  `synchronous=NORMAL` and foreign-key checks.
- Post and feed reads use SQLite's internal B-tree API to avoid SQL overhead on the
  fixed schema. Each request opens a fresh SQLite read snapshot, including feed
  ordering, author names and current like counts. Prepared SQL handles writes.
- A small VFS wrapper coalesces adjacent WAL writes in a bounded buffer. A
  background `PASSIVE` checkpoint moves committed pages into the database and
  advises Linux to evict checkpointed WAL pages from its file cache.
- SQLite's own page cache holds database pages. There are no response, query-result
  or JWT-verification caches. Every authenticated request verifies its signature.
- Telemetry is compiled out by default (`TDS_TELEMETRY=off`).

The SQLite VFS uses `unix-excl` so the API and checkpoint handles coordinate
locks in-process. Stop the server before opening its database with external
SQLite tools. The B-tree reader is tied to the pinned SQLite version and the
challenge's unchanged schema.

## Tests

Build and run the native tests from this directory:

```bash
bash build.sh --tests
ctest --test-dir .build --output-on-failure
```

Challenge API suite (from the repository root):

```bash
bash test/run.sh submissions/cpp-iouring-smolblackhole
```
