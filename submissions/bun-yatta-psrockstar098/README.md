# bun-yatta-psrockstar098 — Yatta JS entry

A 5-endpoint feed API (see `SPEC.md`) implemented with
[Yatta JS](https://yatta.js.org) (`yatta.js@1.0.1` on npm), served by Bun.

## Stack

- Language: TypeScript (run directly by Bun, no transpilation step)
- Framework: Yatta JS 1.0.1 — HTTP routing via `yatta.js/api`
  (`createAPI`/`API`), SQLite via `yatta.js/db` (`createDatabase`)
- Driver: `bun:sqlite` (the driver Yatta's DB layer itself is built on)
- Runtime: Bun 1.4.4 (pinned, official release zip, SHA-256 verified in
  `install.sh`)
- Database: SQLite, the file at `$SQLITE_PATH`
- Front: **direct** — the server binds `$HOST:$PORT` itself (no nginx).
  Bun's HTTP server holds the keep-alive connections; `idleTimeout` is set
  to 120 s, above the 65 s the challenge requires for idle connections.

## How it works

- `src/server.ts` is the whole app. Routes are registered on Yatta's API
  router (`GET /health`, `GET /feed`, `GET /posts/:id`,
  `POST /posts`, `POST /posts/:id/like`) and served by `Bun.serve`
  with `fetch: (req) => api.handle(req)`.
- The database is opened through Yatta's `createDatabase({ path })` with
  **no schema option**, so Yatta never runs its schema sync against the
  challenge database — the fixed schema is used exactly as given. Yatta
  applies `journal_mode = WAL`, `synchronous = NORMAL` and
  `busy_timeout = 5000` on the connection, which is what the challenge
  requires for durable writes.
- All queries go through Yatta's `rawAll`/`rawGet`/`rawRun` helpers, which
  reuse prepared statements (explicitly allowed by rule 5). Every request
  reads its data from SQLite while it is being served: there is no
  response cache, no query-result cache, no in-memory copy of any table,
  and every write is committed before the response is sent.
- Auth is HS256 JWT verification done in `src/server.ts` with
  `node:crypto` (signature + `exp` checked on every request, nothing
  about a token cached). Yatta's own auth subsystem is a full accounts
  system whose error strings do not match `SPEC.md`, so it is not used
  here; the error strings and statuses are exactly the spec's, including
  auth-before-id ordering on the like route.
- One deliberate adapter: Yatta's router spells its default 404
  `{"error":"Not Found"}` while the spec requires lowercase
  `{"error":"not found"}`. The serve wrapper rewrites only that exact
  default body; handler 404s (`post not found`) pass through untouched.
- This app uses Yatta's library modules (`yatta.js/api`,
  `yatta.js/db`), not the full Yatta cluster runtime — the runtime's
  worker pools and subsystems (jobs, mail, storage, realtime) would be
  dead weight on a 1 vCPU box and are not needed for five endpoints.

## Optimizations (and why)

- Prepared statements for every query (Yatta's statement cache): the
  feed query is the hot path and preparing it once removes per-request
  parse overhead.
- Single SQLite connection, synchronous queries, no awaits between a
  request's queries: no interleaving, no pool, no lock contention on one
  core.
- Compact JSON via `JSON.stringify` with spec-ordered keys, so golden
  responses match byte-for-byte with no post-processing.
- `idleTimeout: 120` so keep-alive connections survive the load test's
  think/idle gaps without reconnect churn.
- Compiled binary (`bun build --compile`): the server starts in well
  under the 60 s health window with no module-resolution cost at boot.

## Scripts

- `install.sh` — runs once as root on clean Ubuntu 24.04: installs
  `ca-certificates`, `curl`, `unzip`, then Bun 1.4.4 from the official
  release zip (SHA-256 verified, AVX2 build when the CPU has it,
  baseline build otherwise).
- `build.sh` — runs as a normal user: `bun install` (pinned by
  `bun.lock`), then `bun build --compile src/server.ts --outfile
  bin/server`.
- `start.sh` — runs `./bin/server` in the foreground, configured only
  by `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`.
