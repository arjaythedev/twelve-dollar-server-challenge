# Dart, hand-rolled HTTP/1.1 on `dart:io` sockets, SQLite via FFI

| | |
|---|---|
| Language | Dart 3.13.2, AOT-compiled (`dart compile exe`) |
| Framework | none: a small HTTP/1.1 parser on `dart:io` `ServerSocket` (no `HttpServer`) |
| SQLite driver | [`sqlite3`](https://pub.dev/packages/sqlite3) 2.4.7 (FFI) against Ubuntu's `libsqlite3` |
| Other deps | [`crypto`](https://pub.dev/packages/crypto) 3.0.6 for HMAC-SHA256 |
| Nginx or direct | **Direct.** Run with `HOST=0.0.0.0 PORT=80`. Nginx would take a share of the single vCPU, and the Dart event loop (epoll) handles ~15k idle keep-alive sockets fine. It also works behind `bench/nginx.conf` on `127.0.0.1:3000`: idle connections are never closed by the server. |

Versions are pinned in `install.sh` (Dart SDK) and `pubspec.yaml`/`pubspec.lock` (`pub get --enforce-lockfile`).

## Optimizations

- **AOT native binary** instead of the JIT VM: fast startup, lower memory, no warm-up.
- **No `dart:io` `HttpServer`.** It allocates a lot per request (header maps, streams, `HttpRequest`/`HttpResponse`
  objects). This server parses the request line and only the three headers it needs (`Content-Length`,
  `Authorization`, `Connection`) straight from the byte buffer. It supports pipelining, and it writes each
  response as one pre-sized byte buffer with `TCP_NODELAY`. Static error responses are pre-encoded once.
- **One isolate, one SQLite connection (opened without mutexes), synchronous FFI calls.** On a 1-vCPU box, extra isolates only add
  context switches and memory. SQLite calls are microseconds, so doing them inline on the event loop is
  cheaper than any thread hand-off. Writes are serialized for free, with no `SQLITE_BUSY`.
- **Zero-copy JSON for posts.** Text columns are selected as `CAST(... AS BLOB)`, so the raw UTF-8 bytes
  go straight from SQLite into a reusable output buffer with byte-level JSON escaping. This avoids decoding
  to Dart `String`s, `jsonEncode` and re-encoding to UTF-8. It took `GET /feed` from 13.5k to 23k req/s on
  one core (+70%) and `GET /posts/:id` from 41k to 50k.
- **Persistent prepared statements**, reused for every request (they cache only the query plan).
- **Pragmas:** WAL + `synchronous=NORMAL` (allowed by rule 6; each INSERT is its own committed transaction
  before the 201 goes out), a 256 MB page cache and a 512 MB `mmap`, so the ~250 MB database is served from
  memory after warm-up.
- **Feed in one query:** the reference SELECT with the correlated `count(*)` per post, which uses
  `likes_post_id_idx`, and the `posts_created_at_id_idx` index for the ordering.
- **Like** = an existence check on the primary key, then `INSERT … ON CONFLICT DO NOTHING`. `changes()` tells
  201 from 200, with no extra SELECT.
- **JWT is verified on every request** (HMAC-SHA256, constant-time compare, `alg` must be `HS256`, `exp` checked).
  Nothing is cached across requests.

## Local numbers (server pinned to one core with `taskset -c 0`, autocannon on other cores)

| Endpoint | Req/s | Connections |
|---|---|---|
| `GET /feed` | ~23k | 200 |
| `GET /posts/:id` | ~49k | 200 |
| `POST /posts/:id/like` | ~31k | 200 |
| `POST /posts` | ~20k | 100 |

RSS stays at about 55–80 MB under load (5,000 connections included), plus the SQLite page cache as it fills.

Tried and dropped, because they made no measurable difference: iterating rows with a cursor instead of a
ResultSet, a custom SQLite build (`THREADSAFE=0`, `DEFAULT_MEMSTATUS=0`, …), and a larger Dart young generation.
