# PHP + pype (a full framework) on OpenSwoole

The API written the way a [pype](https://github.com/Valourite/pype-framework) app is meant to be
written: routes with per-route stage lists, the framework's query builder, validation, auth guard,
JSON outlet and error handling. Not a hand-rolled `server.php`.

| | |
|---|---|
| Language | PHP 8.3.6 (Ubuntu 24.04's `php8.3-cli`), opcache + tracing JIT |
| Framework | [pype/framework](https://packagist.org/packages/pype/framework) v0.12.0 (MIT), pinned in `composer.lock` |
| Server | OpenSwoole 26.2.0 `OpenSwoole\Http\Server`, built from the source tarball in `install.sh` (sha256 pinned) |
| SQLite driver | `pdo_sqlite` (`php8.3-sqlite3`), Ubuntu's SQLite 3.45.1 |
| Dependencies | pype/framework, psr/container, psr/log, psr/simple-cache, dragonmantank/cron-expression (all from `composer.lock`); Composer 2.11.1 (sha256 pinned) |
| **Nginx or direct** | **Direct**: OpenSwoole serves `HOST:PORT` (`0.0.0.0:80` on the box) itself |

## Running it

```bash
sudo bash install.sh   # apt: php8.3 cli/dev/sqlite3/mbstring/opcache; builds openswoole.so; installs composer
bash build.sh          # composer install --no-dev --classmap-authoritative; php bin/pype optimize (route cache)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`bash test/run.sh submissions/<this folder>` passes 42/42 (checked in a clean `ubuntu:24.04` container).

## How it's built

| Spec | pype feature used |
|---|---|
| routes, auth before id | `routes/api.php`: a route group with the `auth` stage, then `ValidPostId`, then the stage that does the work |
| JWT auth, 3 different 401s | `App\Auth\ChallengeJwtGuard`, a pype `Guard`: HS256 only, `hash_hmac` + `hash_equals`, `exp`, on every request. pype's own `Jwt` (lcobucci/jwt) refuses HS256 keys under 32 bytes, and the challenge's secret is 23 |
| body rules + messages | `TrimStrings` (global) + `ValidatePost` (a `Validate` stage): `required\|string\|max:500`, status 400 |
| malformed JSON, no route, 500 | `JsonBody` (global) and the framework's error keys, worded in `config/app.php` `errors` |
| post objects | `App\Repositories\Posts`: the query builder (join + like-count subquery) hydrating `App\Data\Post`; new posts with `insertReturning()` |
| like once per user | `INSERT ... ON CONFLICT DO NOTHING`; the affected row count decides 201/200 |
| `{"posts":...}` / `{"error":...}` | `App\Outlets\ApiOutlet` overrides `JsonOutlet::payload()` |

## Optimizations, and why

- **OpenSwoole, 1 worker, base mode** (`SWOOLE_MODE=base`, set in `start.sh`). The app boots once;
  the single worker accepts and serves its own connections, so there is no reactor-to-worker hop.
  Base mode used ~20% less CPU per request than process mode here; 2 workers were slower than 1
  on one vCPU. Handlers are synchronous (no coroutines): every query is an index lookup.
- **Direct instead of Nginx**: OpenSwoole's epoll reactor holds the idle keep-alive connections.
  `backlog` 8192.
- **opcache + tracing JIT** (`start.sh` `-d` flags). Without opcache PHP skips its opcode optimizer
  (~10% more CPU per request even though a worker compiles each file once); the JIT saves another
  ~5-10%.
- **`memory_limit=-1`**: in base mode OpenSwoole counts a 64 KB receive buffer per connection
  against PHP's limit, so a finite one kills the worker at a few thousand idle connections. The box's
  2 GB is the real limit; RSS was ~600 MB at 68,000 users.
- **Framework**: `composer --classmap-authoritative`, `bin/pype optimize` (cached routes), only the
  Log, Database and Auth providers loaded, logging at `warning`.
- **SQLite**: pype's defaults (WAL, `synchronous=NORMAL`, `busy_timeout`, 20 MB page cache,
  `temp_store=MEMORY`) plus `mmap_size` 256 MB. pype's `Connection` keeps each prepared statement for reuse (the plan, not the data).
  No caching across requests (rule 5).

## Changes since #335

#335 (pype 0.12.0) scored 36,000 users, CPU-bound at 246 µs per request. This follow-up runs pype
0.13.1, whose per-request work was cut after profiling this app (excimer, the challenge's request
mix): rows hydrate through PHP's own type coercion instead of a cast per value, the container
reflects each constructor once, the stage pipeline runs by index instead of nested closures, and the
static-file check is skipped for extension-less paths. The app itself is unchanged except that the
challenge's secret moved to `auth.challenge.secret` (0.13 refuses a short `auth.jwt.secret` at boot).

CPU per request, this folder's scripts in an `ubuntu:24.04` container (`--cpus 1`, fixed 8,000
req/s of the challenge's mix, four alternating runs each on a busy host): **0.12.0: 61 µs,
0.13.1: 55.5 µs (−9%)**.

## Measured locally

With pype 0.12.0 (#335). A `docker run --cpus 1 --memory 2g --ulimit nofile=65535` container built from
these scripts (Ubuntu 24.04, PHP 8.3), k6 on the same 16-core host. Not the benchmark box: there the
same code was CPU-bound at 36,000 users, so treat these as indicative.

| test | result |
|---|---|
| `bench/load.js`, **68,000 users, 5 min hold** | **PASS**: p95 0.5 ms, p99 4.4 ms, 0.73% errors, CPU 34%, 612 MB |
| `bench/load.js`, 69,000-71,000 users | FAIL on errors (1.1-1.35%), latency unchanged |
| fixed request rate, challenge request mix | 24,000 req/s passes at 91% CPU (p95 6.9 ms); 20,000 at 75% |

Locally the limit was the 65,535 open files (past ~65k users OpenSwoole logs `accept() failed,
Error: Too many open files`), because this host's CPU is ~5x faster than the benchmark box's.
