# BinaryCore — C, raw io_uring, SQLite

One event-loop thread serves every connection through raw Linux io_uring (no
liburing) and every request through one SQLite transaction per batch (group
commit). Reads use standard prepared SQL; writes are durable (WAL +
`synchronous=NORMAL`) before the response is sent.

| | |
|---|---|
| Language | C11, gcc (Ubuntu 24.04 `build-essential`) |
| Framework | none: own io_uring layer (raw syscalls), HTTP/1.1 framing, JSON, HS256 |
| Database | SQLite 3.54.1 amalgamation, SHA-256 pinned, downloaded by `build.sh` |
| Build | `-O3`, static, `march=x86-64-v3` |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Target | Ubuntu 24.04 x86_64, 1 vCPU, 2 GB RAM |

## Requirements

- `install.sh` — run once as root on clean Ubuntu 24.04: `build-essential`,
  `curl`, `ca-certificates`, `libsqlite3-dev`, `sqlite3`, `unzip`.
- `build.sh` — builds `bin/server` as a normal user. Downloads the pinned
  SQLite 3.54.1 amalgamation (SHA-256 verified) and compiles it together with
  the server as one translation unit.
- `start.sh` — runs `bin/server` in the foreground. Configured only by
  `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.

## API

Exactly HTTP/1.1 + JSON per `SPEC.md`:

- `GET /health` → `{"status":"ok","db":"ok","uptime_s":<int>}`
- `GET /feed` → `{"posts":[<post>×20]}` newest 20
- `GET /posts/:id` → `{"post":<post>}`
- `POST /posts` (auth) → `{"post":...}` 201
- `POST /posts/:id/like` (auth) → `{"liked":true,"already_liked":<bool>,"post_id":<int>}`

Auth: `Authorization: Bearer <jwt>`, HS256 with `JWT_SECRET`, verified on every
request (constant-time signature compare, `exp` checked, `sub`/`username`
validated). No token caching.

## Design

- **Networking**: raw io_uring, multishot accept + multishot poll + provided
  buffer rings, direct descriptors (bypass the 65,535 fd limit), `MSG_DONTWAIT`
  reads, one read per connection per batch, bounded read queue (drain-first to
  avoid `ENOBUFS` storms).
- **Database**: SQLite WAL + `synchronous=NORMAL` + `wal_autocheckpoint=0`,
  prepared statements, group commit (one `BEGIN`/`COMMIT` per batch; each
  response waits for its batch's commit), background checkpoint thread.
- **JSON**: validating reader + compact SIMD-escaped writer, no allocation.
- **JWT**: SHA-256/HMAC with precomputed key schedule, constant-time compare.
- **Memory**: arena allocation with refcounted batch arenas, fixed connection
  pools, memory pre-warming, no per-request `malloc`/`free`.

## Build and run

```bash
# As root, once:
bash install.sh

# As a normal user:
bash build.sh

# Run:
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

## Testing

```bash
bash seed/make-seed.sh                      # once, to build seed/feed.db
bash test/run.sh submissions/c-iouring-binarycore-zekridev1
```

## Known limitations

- This implementation uses standard SQLite prepared statements (not the
  B-tree cursor reads used by the top leaderboard entries). That sacrifices
  some per-request CPU for correctness simplicity. B-tree reads are the
  highest-reward optimization to add next.
- Developed on a Windows host; the Linux build and the official test suite
  must be run on the Ubuntu 24.04 target environment before any performance
  claims are validated.

## Compliance

- Only files inside `submissions/c-iouring-binarycore-zekridev1/` are added or
  modified. No official challenge files, other submissions, schema, tests, or
  leaderboard are touched.
- SQLite is the only database; the schema is fixed; no cross-request caching;
- writes are durable before response; every JWT is verified per request.
