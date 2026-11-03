# PHP + Symfony on FrankenPHP

| |                                                                                              |
|---|----------------------------------------------------------------------------------------------|
| Language | PHP 8.5.12, the interpreter embedded in the official FrankenPHP static build (glibc variant) |
| Framework | Symfony 8.1.8: FrameworkBundle + SecurityBundle, nothing else                                |
| Server | FrankenPHP 1.13.0 (Caddy 2.11.7) in worker mode                                              |
| SQLite driver | `pdo_sqlite` through Doctrine DBAL 4.5, SQLite 3.45.2 as bundled in the binary               |
| JSON | `json_encode`                                                                                |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself                                                       |

## Running it

```bash
sudo bash install.sh   # FrankenPHP static binary -> /usr/local/bin/frankenphp, and Composer -> /usr/local/bin/composer
bash build.sh          # composer install + Symfony cache:warmup (container, routes, preload file...)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

No PHP is installed from apt: Composer and `bin/console` run through `frankenphp php-cli`, so the build uses exactly the PHP that
serves the requests.

## Why this stack

In the video PHP ran the classic way: php-fpm boots the framework from scratch on every request. That bootstrap, not
the language, is the cost. FrankenPHP's worker mode removes it: `public/index.php` boots the kernel once and then loops on
`frankenphp_handle_request()`, so a request is a routed controller call on an already-built container. The point of this
submission is to see how far a current, idiomatic PHP stack gets once that one thing is fixed,
rather than how fast a hand-rolled PHP socket server can be.

## Optimizations, and why

- **Direct instead of Nginx.** Caddy is already the HTTP server, so Nginx in front would be a second proxy on the same single
  core. Caddy (Go) holds the ~15,000 keep-alive connections itself, with HTTP/1.1 only, no access log, no compression, no admin
  API and `auto_https` off.
- **Two PHP worker threads.** On one vCPU more threads only add context switches, but a second worker keeps the CPU busy while the
  first one is inside a write. Measured at 15,000 users on one pinned core (below): 1 worker p99 763 ms, 2 workers 401 ms,
  4 workers 1.06 s. Each worker has its own SQLite connection.
- **One SQLite connection per worker, opened once, with every prepared statement reused.** `Db` opens the connection on first
  use and keeps it for the life of the worker; `PostRepository` prepares each of the five queries once. That caches query plans,
  never rows (via rule 5). Every result is freed as soon as its rows are read: an un-reset statement keeps SQLite's implicit
  transaction open, which would pin the connection to a stale WAL snapshot (the other worker's writes become invisible, and the
  next write fails with `SQLITE_BUSY_SNAPSHOT`); for `INSERT ... RETURNING` it is also the reset that commits, so freeing the
  result before answering is what makes the 201 mean "committed" (rule 6).
- **Pragmas** (set once per connection): `journal_mode=WAL`, `synchronous=NORMAL`, `busy_timeout=10000` so the two
  workers queue on the write lock instead of failing, `mmap_size=1 GiB`, `cache_size=64 MiB`, `temp_store=MEMORY`. Autocommit:
  every write is committed before the response is built.
- **SQL**: the reference queries. A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`;
  only when it inserts nothing does a second lookup tell "already liked" from "no such post". A post is
  `INSERT ... RETURNING id, created_at`, so the response needs no second query.
- **Symfony Security does the authentication**, the idiomatic way: a stateless firewall on `POST /posts` and `POST /posts/:id/like`
  with the `access_token` authenticator, a custom `AccessTokenHandlerInterface` that verifies the JWT, an entry point for the
  missing-token case and a failure handler for rejected tokens, each answering with the exact spec message. Because the
  firewall runs before routing arguments are resolved, "auth first, then the id" comes for free. `GET` routes are outside the
  firewall pattern, so public reads never enter the security layer.
- **JWT verified by hand on every request**: HS256 only (`alg` must be `HS256`, so `none` is rejected), `hash_hmac` +
  `hash_equals`, `exp` and `nbf` checked with zero leeway, nothing cached between requests. Not a existing library because both
  firebase/php-jwt 7 and lcobucci/jwt 5 refuse HMAC keys under 256 bits, and the challenge secret is 23 bytes.
- **Lean Symfony.** Controllers are plain invokable-style services (`#[AsController]`, no `AbstractController`), responses are
  `Response` objects with pre-encoded compact JSON (`JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE`, keys in spec order), request
  bodies are decoded with `json_decode` and validated by hand so the error messages are byte-exact. No Serializer, Validator,
  sessions, CSRF, Twig or Monolog in the container. A `kernel.exception` listener turns unknown paths into `404 {"error":"not found"}`
  and anything unexpected into `500 {"error":"internal server error"}`.
- **OPcache JIT on** (`opcache.jit=tracing`, 64 MB buffer): +17% requests per second in a no-think-time run, and at 15,000 users
  p95 went from 215 ms to 99 ms. `GOGC=200` for the Go side was tried and dropped: 200 MB more RSS and a worse p99.
- **Build-time work.** `build.sh` runs `composer install --classmap-authoritative` and `cache:warmup`, so the compiled container,
  the route matcher and the OPcache preload file exist before `start.sh`. OPcache runs with `validate_timestamps=0` and the preload
  file, so no `stat()` calls on PHP files at runtime. (`opcache.preload` needs a non-root user, which is how the benchmark service
  runs.)
- **Worker hygiene.** Symfony resets its per-request services at the start of each `handle()`, `gc_collect_cycles()` runs after
  each response, and `MAX_REQUESTS` (unset: unlimited) can recycle a worker. `GODEBUG=cgocheck=0` as the FrankenPHP performance guide
  recommends.

## What it did on my machine

Not the benchmark box: a 4-core container with the server pinned to one core with `taskset` (k6 on the other three, so the
kernel's network work for the server was not confined to that core, which flatters these numbers). `bench/load.js` with a 30 s
ramp and a 60 s hold on a fresh copy of the seed, 2 workers, JIT on. The file limit here was 20,000, so 15,000 users was the top.

| Users | p95 | p99 | Errors | Server CPU (of one core) | RSS |
|---|---|---|---|---|---|
| 2,500 | 3 ms | 6 ms | 0 | ~22% | 230 MB |
| 5,000 | 4 ms | 10 ms | 0 | ~38% | 345 MB |
| 10,000 | 10 ms | 67 ms | 0 | ~58% | 575 MB |
| 15,000 | 99 ms | 401 ms | 0 | ~71% | 770 MB |

Memory is almost all Caddy's per-connection buffers (about 45 KB per idle keep-alive connection); the two PHP workers are a
constant ~110 MB. Without think time (50 users hammering the four endpoints, 10% creates) one core serves about 2,400
requests per second.

## License

MIT, under the repo's [license](../../LICENSE).
