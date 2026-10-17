# Bun + Elysia 2

| | |
|---|---|
| Runtime | Bun 1.4.2 (official release zip, SHA-256 checked) |
| Language | TypeScript |
| Framework | Elysia 2.0.0-beta.31, validation with TypeBox 1.3.37 (`t` from Elysia) |
| SQLite driver | `bun:sqlite` (built into Bun 1.4.2, SQLite 3.53.2 statically linked) |
| Build | `bun build --compile --production`: one native executable, `bin/server`, fully minified |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions, transitive ones included, are pinned in `bun.lock` (`bun install --frozen-lockfile`).

## Running it

```bash
sudo bash install.sh   # apt: ca-certificates, curl, unzip; Bun 1.4.2 to /usr/local/bin/bun
bash build.sh          # bun install --frozen-lockfile --production, then bun build --compile --production -> bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Configuration

| Variable | Default | |
|---|---|---|
| `SQLITE_PATH` | required | Database file (spec) |
| `JWT_SECRET` | required | HS256 secret (spec) |
| `HOST` | `127.0.0.1` | Listen address (spec) |
| `PORT` | `3000` | Listen port (spec) |
| `MEMORY_SOFT_LIMIT_RATIO` | `0.75` | Share of the available memory above which the memory guard forces a GC, in (0, 1] |
| `MEMORY_CHECK_INTERVAL_MS` | `2000` | How often the memory guard reads RSS |
| `MEMORY_GC_COOLDOWN_MS` | `30000` | Minimum time between two forced GCs |

The benchmark box sets only the spec variables, so the memory guard runs with its defaults there.

## Layout

Clean Architecture, one module per feature, written as plain functions (factories and closures):

```
src
├── server
│   ├── main.ts                bootstrap: listen on HOST:PORT
│   ├── api.ts                 composition root: wires repositories -> use cases -> route plugins
│   ├── config/                env, SQLite connection and pragmas
│   ├── runtime/               production mode for Elysia, RSS memory guard
│   └── middlewares/           auth (derive) and the global error handler
└── modules
    ├── auth                   domain (user, errors, verifier contract), use case, HS256 verifier
    ├── health                 domain, use case, SQLite repository, route plugin
    └── posts                  domain (post, body rules, errors, repository contract),
                               use cases (feed, get, create, like), SQLite repository, DTOs, presenter, routes
```

Domain and use cases know nothing about HTTP or SQLite. The HTTP adapters validate input with TypeBox
DTOs and map domain objects (camelCase) to the wire format (`created_at`, `like_count`, …) in presenters.
Domain errors are values, not exceptions: use cases return them and each route maps them to a status
code. `src/server/middlewares/errorHandler.ts` only handles framework errors (validation, malformed JSON,
unknown route) and unexpected failures.

## Optimizations, and why

- **One process, one SQLite connection, queries inline on the event loop.** The box has one vCPU and
  Bun runs JavaScript on a single thread, so workers would only add context switches. Every query is a
  primary-key or index lookup; one connection also means writes never wait on SQLite locks.
- **Direct instead of Nginx.** On one core, Nginx's CPU would come out of the app's budget. Bun's HTTP
  server holds tens of thousands of idle keep-alive sockets cheaply. The idle timeout is raised to
  120 s (Bun's default is 10 s), so a user's connection survives the 5–23 s think time between requests
  instead of being closed and reopened. In a local A/B with 10,000 users, with Nginx (`bench/nginx.conf`)
  sharing the same CPU budget, total CPU was the same and p99 went from 13.5 ms (direct) to 39.8 ms.
- **Production build.** `bun build --compile --production` makes one fully minified executable, so startup
  does no module resolution or transpiling. Elysia reads `NODE_ENV` from `process.env` at runtime, which the
  build-time define does not reach, so `runtime/production.ts` is the first import and sets it before Elysia
  loads: production mode compiles routes once and drops a per-request path check.
- **Memory guard instead of `--smol`.** `--smol` would shrink the heap and collect more often on every
  request path. Instead, a timer compares `process.memoryUsage.rss()` with a soft limit, a share of
  `process.constrainedMemory()` (Bun returns the cgroup limit when there is one, the machine's memory
  otherwise), and forces a full GC only above it, with a cooldown, so the 2 GB box never runs out of memory.
- **Dependencies injected by closure, not per request.** Use cases and repositories are built once in
  the composition root and captured by the route handlers, so nothing is added to each request's
  context.
- **No exceptions on expected failures.** Domain errors (bad token, missing post, invalid body) are
  preallocated once and returned, not thrown: no allocation, no stack capture, no unwinding and no trip
  through Elysia's error hook. The success path pays nothing extra: no `Result` wrapper object.
- **Elysia, no third-party plugins.** Each feature is a plain Elysia instance mounted with `.use()`, and
  Elysia compiles each route's lifecycle into one specialized function. Params
  and body are validated by compiled TypeBox schemas that carry the spec's error messages. The like
  route sets `parse: 'none'`, so its empty body is never read.
- **Auth before the id.** In Elysia 2 a `derive` runs after schema validation, so the like route has no
  params schema: it checks the id by hand, with the same pattern as the DTO, right after the auth derive.
  `POST /posts/abc/like` without a token is a 401, with a valid token a 400.
- **JWT verified by hand on every request** (no caching, rule 5): HS256 only, HMAC with a
  `Bun.CryptoHasher` keyed once and copied per request (no re-keying), constant-time compare with Bun's
  global `crypto.timingSafeEqual`, `exp`/`nbf` checked. The
  canonical `{"alg":"HS256","typ":"JWT"}` header is matched as a string, so only other headers get parsed.
- **SQL**: the reference queries, prepared once and reused. Likes are one statement,
  `INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING`;
  only when it inserts nothing does a second lookup tell "already liked" from "no such post".
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 64 MiB page
  cache, `temp_store=MEMORY`. Autocommit commits each write before its response is sent.

## License

MIT, under the repo's [license](../../LICENSE).
