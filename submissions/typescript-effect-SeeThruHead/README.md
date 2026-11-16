# TypeScript + Effect

| | |
|---|---|
| Language | TypeScript, run directly by Node.js 24.21.0 (built-in type stripping, no build step) |
| Framework | [Effect](https://effect.website) 4.0.4: `effect/http` router on `@effect/platform-node` 4.0.4 |
| Database access | drizzle-orm 0.45.4 query builder on better-sqlite3 13.0.3 (bundled SQLite 3.53.4) |
| Package manager | pnpm 10.33.0, lockfile committed |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## What this is

This is meant to approximate a service I would actually ship and maintain in production, not the
fastest thing that passes the tests. It is a normal Effect application on a normal HTTP server, and
the numbers below are what that costs and buys on one vCPU.

Things it deliberately does **not** do, even though they would make it faster:

- No SQLite-generated JSON and no piping bytes from SQLite to the socket. Rows are decoded into typed
  values and responses are encoded by schemas, so the API shape lives in the application.
- No raw TCP sockets or hand-written HTTP parsing. It uses Effect's HTTP router on Node's `http` server.
- No patched, profile-guided or custom-compiled SQLite, no io_uring, no native addons of my own.
- No custom memory pools or manual buffer reuse.
- No response caches or cached token verifications.

- **Typed errors end to end.** Every failure is a `Schema.TaggedError`. Repository errors are mapped
  into service errors, and each error renders its own HTTP response (`HttpServerRespondable`), so the
  status and body for every case in the spec are checked by the compiler, not by string matching.
- **Schema at every boundary.** Path params, the `Authorization` header, JWT segments, request
  bodies, rows coming back from SQLite and every response body go through `effect/Schema`.
- **Services and layers.** `Database`, `PostRepo`, `Posts` and `Auth` are `Context.Service`s with
  layers; the HTTP server and SQLite driver are platform layers chosen in the entrypoint.

## Running it

```bash
sudo bash install.sh   # Node.js 24.21.0 (checksum-verified tarball) + pnpm 10.33.0
bash build.sh          # pnpm install --frozen-lockfile --prod
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design and performance choices

- **One connection, prepared once.** A single better-sqlite3 connection is opened for the life of the
  process. Access is serialized with an Effect `Semaphore` (the same approach as Effect's own SQLite
  client), and every statement is a drizzle prepared statement built once at startup and reused.
- **Group commit with Effect's request batching.** Post and like writes are `Request`s resolved by a
  `RequestResolver`. Writes that arrive together are applied in one `BEGIN IMMEDIATE` transaction,
  and every caller gets its response only after that transaction commits (rule 6).
- **Pragmas:** WAL, `synchronous = NORMAL`, 64 MiB page cache, 512 MiB `mmap_size`, in-memory temp store.
  `NORMAL` is what the rules allow and what most SQLite production setups use, but a committed
  transaction can be lost on sudden power loss. Because writes are group-committed, switching to
  `FULL` would cost one fsync per batch rather than per request.
- **JWT** is verified on every request (HS256 only, constant-time signature comparison, `exp` checked
  against Effect's `Clock`), using `node:crypto`.
- **Direct instead of Nginx.** On one vCPU, Nginx's CPU comes out of the app's budget. Keep-alive
  connections are held for 75 s.
- **Swappable runtime.** App code only depends on `effect/http` and drizzle's sync SQLite interface.
  `src/main.node.ts` wires Node + better-sqlite3; `src/main.bun.ts` wires Bun + `bun:sqlite` the same
  way. The submission runs on Node; the Bun entrypoint typechecks but has not been benchmarked yet.

## Local results

Measured with `bench/load.js` (60 s ramp, 2 min hold) against an Ubuntu 24.04 container limited to
1 CPU and 2 GB with no swap, built with this folder's `install.sh` and `build.sh`, with k6 in a
separate container on the same machine (Apple silicon, so these numbers are only indicative):

| Users | Failed requests | p95 | p99 |
|---|---|---|---|
| 10,000 | 0% | 4.2 ms | 11.5 ms |
| 15,000 | 0% | 3.9 ms | 25 ms |
| 17,500 | 0% | 5.1 ms | 35 ms |

At 20,000 users the run failed the latency thresholds locally. k6 shared the machine and was using
most of its cores at the same moments, so I could not separate the server's limit from the load
generator's.

## License

MIT, under the repo's [license](../../LICENSE).
