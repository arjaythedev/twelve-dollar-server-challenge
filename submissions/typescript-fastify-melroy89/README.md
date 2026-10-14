# TypeScript / Fastify

| | |
|---|---|
| Language | TypeScript 7.0.2 on Node.js 26.11.1 (official x86_64 archive) |
| Framework | Fastify 5.12.7 |
| Authentication | `@fastify/jwt` 10.2.4 (HS256 verification on every request) |
| SQLite driver | `better-sqlite3` 13.0.3, compiled from source; SQLite 3.53.4 |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself, without Nginx |

All direct and transitive npm dependencies are pinned in `package-lock.json`.

Direct HTTP/1.1 on `HOST:PORT`, without Nginx. One Node.js process and one
in-process SQLite connection. The application reads only `SQLITE_PATH`,
`JWT_SECRET`, `HOST` and `PORT` for configuration.

## Stack

- Node.js **26.11.1** (official x86_64 archive, SHA-256 checked by `install.sh`).
- Fastify **5.12.7**, `@fastify/jwt` **10.2.4**.
- `better-sqlite3` **13.0.3** / SQLite **3.53.4**, compiled from source by
  `build.sh`. The application explicitly loads that build rather than the
  package's bundled prebuilt binding.
- TypeScript **7.0.2**; exact dependency versions and transitive integrity hashes
  are recorded in `package-lock.json`.

`install.sh` installs the runtime and compiler tools as root on Ubuntu 24.04.
`build.sh` installs locked dependencies, builds the SQLite binding as a normal
user, and compiles TypeScript. `start.sh` runs the server in the foreground.
No generated binaries or dependency directories are submitted.

## Optimizations and correctness

- Prepared statements are reused. Every request executes SQLite queries; no
  response, query-result, table or JWT-verification cache exists.
- Reads use the supplied ordering index, primary keys and covering likes index.
  SQLite constructs each feed entry with `json_object`; the feed concatenates those
  freshly generated JSON objects in query order. This avoids allocating and
  serializing twenty JavaScript objects per feed, while retaining JSON escaping
  and the exact API property order. Single-post reads and create responses use
  Fastify response schemas, which measured faster for the single-post route.
- WAL, `synchronous=NORMAL`, foreign keys enabled, a 64 MiB SQLite page-cache
  budget and up to 256 MiB of SQLite mmap. The supplied schema is unchanged.
- Pending creates and likes share bounded transactions, up to 128 writes per
  batch, flushed on the next event-loop turn. Successful responses are sent
  after COMMIT. Statement constraint errors stay isolated; transaction-level
  failures fail the whole batch. Likes use
  the existing unique constraint to make duplicate concurrent likes idempotent.
- JWT signatures and expiration are verified on every authenticated request,
  restricted to HS256 with verifier caching explicitly disabled. Authentication
  runs before body parsing and ID validation.
- Ordinary keep-alive lasts 75 seconds. At 60,000 open server connections, or
  a smaller inherited descriptor limit minus 128, completed responses advertise
  `Connection: close`. Uploads finish and successful writes commit before their
  responses retire a connection. Clients reconnect normally. Connection counts
  are transport accounting, not cached application data.
- The process requests a listen backlog of 4,096 to absorb connection bursts;
  no system limit is changed.
- No request logging, compression, generic middleware adapters, external
  services, kernel tuning or CPU affinity. Compression reduced throughput in
  local comparisons and was rejected.

## Validation

From the repository root, after generating the seed:

```bash
bash test/run.sh submissions/typescript-fastify-melroy89
cd submissions/typescript-fastify-melroy89
node verify.mjs ../../seed/feed.db
node verify-sockets.mjs ../../seed/feed.db
```

The additional checks use disposable database copies. They cover fresh external
database changes, invalid JWTs, Unicode lengths, concurrent duplicate likes,
empty JSON-typed likes from the official workload, foreign-key failure recovery,
acknowledged-write persistence through SIGKILL/restart, unchanged schema,
fragmented uploads under `nofile=256`, and original-socket reuse after 66 seconds.

For Ubuntu 24.04 qualification and local capacity measurements:

```bash
docker build -t twelve-fastify-qualification:local .
python3 benchmark.py --vus 90000 --label local-90000
```

The harness runs the repository's unmodified `bench/load.js` with k6 1.4.2.
The server container gets a one-CPU quota, 2 GiB RAM, no swap and `nofile=65535`.
The load generator runs outside that quota, on the same physical host, using
multiple loopback source addresses to avoid local source-port exhaustion.
It performs a 1,000-user two-minute warm-up before the requested 60-second ramp,
five-minute hold and the workload's 30-second ramp-down. Each invocation starts
with a fresh seed copy. Raw summaries, logs, source/workload hashes and peak
cgroup memory are written to ignored `results/`.

The Node.js 26.11.1 local 90,000-user five-minute hold passed: p95 **323.9 ms**,
p99 **732.6 ms**, **0 errors** across **3,348,999 requests**, peak container
memory **1,764.3 MiB**. This is a selected passing load on a Threadripper host,
not maximum capacity or an official DigitalOcean droplet score. See
`PERFORMANCE.md` for the measured comparisons and qualification boundaries.
