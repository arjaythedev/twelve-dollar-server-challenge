# PHP / CodeIgniter 4 — melroy89

A full CodeIgniter **4.7.5** application: CI4 routes, controllers, request/response objects,
production bootstrap and its official FrankenPHP worker lifecycle handle every API request.

| Component | Version |
| --- | --- |
| CodeIgniter | 4.7.5, Composer lockfile committed |
| HTTP server | FrankenPHP 1.13.0, glibc x86_64 runtime with Caddy 2.11.7 |
| Request runtime | PHP 8.5.11 ZTS, bundled with that pinned FrankenPHP release |
| SQLite driver | PDO SQLite; SQLite 3.45.2 bundled with the runtime |
| Build tools | Ubuntu 24.04 PHP 8.3 CLI; Composer 2.8.12 |

**Direct HTTP, without Nginx.** Caddy binds `HOST:PORT` and retains idle HTTP/1.1
connections for five minutes. Only the five challenge routes are exposed; unknown paths
return the specified JSON error. There are no application assets or public directory listings.

## Run

From the repository root:

```sh
sudo bash submissions/php-codeigniter-melroy89/install.sh
bash seed/make-seed.sh
bash test/run.sh submissions/php-codeigniter-melroy89
```

`install.sh` installs Ubuntu build tools and downloads the official pinned runtime and Composer
with SHA-256 verification. `build.sh` installs locked production dependencies, builds an
authoritative Composer classmap and lints the application. No binaries or vendor code are committed.

For a manual disposable instance:

```sh
cp seed/feed.db /tmp/ci4-feed.db
SQLITE_PATH=/tmp/ci4-feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 \
  bash submissions/php-codeigniter-melroy89/start.sh
```

The process stays in the foreground. Restart it after source changes because OPcache timestamp
checks are disabled. `SQLITE_PATH` must name an existing database; startup never creates or seeds one.

## Optimisations and rule compliance

- **Official worker lifecycle.** CI4 boots once per PHP worker, with its request services,
  factories, superglobals and event state reset between requests. Authentication is request-local.
  CI4 labels this worker support experimental; extra isolation checks are included below.
- **Two workers on one CPU.** One and four workers were slower in local diagnostics.
  PHP automatic garbage collection stays enabled; collection is not forced after every request.
  Go uses one scheduler CPU and a 768 MiB advisory heap limit, leaving space for PHP and SQLite.
- **Explicit routing.** Module discovery and controller-attribute scanning are disabled because
  this application uses explicit routes and has no module registrars or attribute filters.
- **Production mode, OPcache and authoritative Composer classmap.** Tracing JIT and realpath
  caching reduce interpreter and file-resolution work. Caddy routes straight to the worker;
  there are no static-file probes, access logs, TLS setup or response compression.
- **Persistent PDO connections and prepared statements.** Only connections and SQL query plans
  survive requests. Every execution reads current SQLite data, and all cursors close immediately
  so workers do not retain stale WAL read snapshots.
- **Fixed schema and indexes.** Feed reads use the supplied ordering index; like counts use the
  supplied post-id index. Likes use a conflict-safe insert, without a preflight existence query
  on the successful path. No tables, indexes, columns or triggers are added or changed.
- **WAL + `synchronous=NORMAL`, foreign keys enabled.** Autocommit writes complete before their
  responses; in particular, the `INSERT ... RETURNING` cursor is finalised before returning 201.
  Busy timeout is five seconds. Each connection uses a 256 MiB mmap allowance and an 8 MiB page cache.
- **No result caching.** CI4 page-cache filters are disabled and the general cache handler is
  `dummy`. Responses, query results and JWT verification are never cached. `spark optimize` is
  deliberately not used: CI4 forbids its disk configuration/locator caches in worker mode.
- **JWT verification on every authenticated request.** HS256 only, constant-time signature
  comparison, expiry and optional `nbf` checks, then strict payload validation. Auth precedes
  like-id validation. Unicode post lengths are counted as characters, rather than bytes.

See [BENCHMARK.md](BENCHMARK.md) for measured tuning results and the boundary between local
measurements and an official challenge score.

## Additional correctness checks

On an already-running instance **with a disposable database copy**, run:

```sh
python3 submissions/php-codeigniter-melroy89/checks/worker.py http://127.0.0.1:3000
```

This writes posts and likes. It checks sequential identity changes over a reused client connection,
expired/malformed claims, Unicode limits, 64 concurrent duplicate likes, fresh cross-worker reads
and recovery after a failed foreign-key write. Run the official suite first against its fresh copy.

## References

- [CI4 deployment guidance](https://codeigniter.com/user_guide/installation/deployment.html)
- [CI4 worker mode and state reset](https://codeigniter.com/user_guide/installation/worker_mode.html)
- [CI4 cache helper](https://codeigniter.com/user_guide/general/common_functions.html#cache)
- [CI4 benchmarking](https://codeigniter.com/user_guide/testing/benchmark.html)
- [FrankenPHP performance guidance](https://frankenphp.dev/docs/performance/)

MIT; see the repository [license](../../LICENSE) and the included CI4 starter license.
