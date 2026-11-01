# PHP + Symfony (FrankenPHP worker mode)

A regular, idiomatic Symfony application, the kind you'd write at work, tuned through configuration and standard
Symfony components first. No custom HTTP server, no hand-written JSON, no group commit.

| | |
|---|---|
| Language | PHP 8.5.11 (ZTS), built into [FrankenPHP's release binary](https://github.com/php/frankenphp/releases/tag/v1.13.0) |
| Framework | Symfony 8.1 (FrameworkBundle 8.1.8, SecurityBundle, JsonStreamer, Runtime for the console) |
| Server | FrankenPHP 1.13.0 (Caddy 2.11.7) in worker mode, with the worker loop of FrankenPHP's docs in `public/index.php` |
| SQLite driver | PDO SQLite (SQLite 3.45.2) via Doctrine DBAL 4.5 (DoctrineBundle 3.3) |
| **Nginx or direct** | **Direct**: FrankenPHP serves `HOST:PORT` itself (`0.0.0.0:80` on the droplet) |

FrankenPHP and Composer are pinned and checksum-verified in `install.sh`, PHP dependencies in `composer.lock`.

## Running it

```bash
sudo bash install.sh   # FrankenPHP's release binary (PHP built in) and Composer, pinned and checksum-verified
bash build.sh          # composer install --no-dev --classmap-authoritative, dump-env prod, cache:warmup
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## The application

- **Routing, controllers**: attribute routes on one `AbstractController`, a `#[ValueResolver]` for post ids (so
  `/posts/abc` is a 400, not a 404), a `kernel.exception` listener for the `{"error":...}` bodies.
- **Auth**: SecurityBundle, a stateless firewall with the `access_token` authenticator, matching only `POST ^/posts`:
  reads never go through it. The JWT is checked on every request by an `AccessTokenHandlerInterface`: HS256 signature
  (`hash_hmac` + `hash_equals`), `exp` and, when present, `nbf`. It's hand-written because the challenge's 23-byte
  secret is shorter than RFC 7518's 256-bit minimum for HS256, which the maintained JWT libraries enforce.
- **Input**: `CreatePostPayload::fromRequest()` decodes and checks `POST /posts`'s body (`json_decode()`, `trim()`,
  `mb_strlen()`), a 400 `BadRequestHttpException` with the spec's message otherwise.
- **Reads**: a read model. `GET /feed` and `GET /posts/{id}` are one SQL query each, through DBAL, and SQLite writes
  the whole response body (`json_object()`, `json_group_array()`), which is returned as is.
- **Writes**: one SQL statement each, through DBAL, autocommitted before the response. A like is
  `INSERT ... SELECT ... ON CONFLICT DO NOTHING` (a second query only tells 404 from 200), a post is
  `INSERT ... RETURNING`. Their responses are written by JsonStreamer from `#[JsonStreamable]` output objects.
  No ORM: the repositories get DBAL's `Connection`.
- **SQLite**: WAL (set in the database file), and a DBAL driver middleware sets `synchronous=NORMAL` and
  `busy_timeout` on each connection. Connections are persistent (`PDO::ATTR_PERSISTENT`), one per PHP thread.
- **Prepared statements are reused** (rule 5: they cache the query plan, not data) by another DBAL driver middleware,
  pooled per SQL string for the life of the driver connection. A statement is checked out while in use, so two users
  of the same SQL never share one, and goes back only once its results are gone, its cursor reset: an SQLite statement
  left mid-result keeps the connection's read snapshot open, and other queries would read stale data.
- **No data is cached across requests**: no Doctrine result cache, and Symfony resets its services between requests.
  What persists across requests is SQLite's page cache and schema, and prepared statements.

## Optimizations, and why

Each change was measured on its own, in both FrankenPHP modes (classic: Symfony boots on every request, like with
PHP-FPM; worker: once per worker), with one commit per change: configuration first, then regular Symfony code, then
the less idiomatic changes. The method, every run, the experiments and two peers are in
[`local-bench/LADDER.md`](local-bench/LADDER.md).

The local stand-in is one core of an Apple silicon Mac, throttled to a quarter (`--cpus=0.25`), with 2 GB (no swap),
in Docker, with k6 on other cores. Users are searched like the real score; requests served per second, saturated,
measure smaller changes. **Its user counts are not droplet scores**: they only compare with each other.

| Phase | Change | Classic users | Worker users | Classic req/s | Worker req/s |
|---|---|--:|--:|--:|--:|
| 0 | Idiomatic Symfony 8.1, FrankenPHP's release binary, defaults | 250 | 1,000 | 39 | 117 |
| 1 | php.ini production settings (OPcache, realpath) | 250 | 1,000 | 41 | 116 |
| 2 | Preloading, authoritative class map | 250 | 1,000 | 48 | 125 |
| 3 | JIT (tracing) | 250 | 1,250 | 50 | 163 |
| 4 | Firewall on `POST /posts` only | 250 | 1,250 | 50 | 162 |
| 5 | Every request to Symfony (`rewrite` + `php`) | 250 | 1,250 | 51 | 164 |
| 6 | DQL read model | 500 | 2,250 | 67 | 273 |
| 7 | JsonStreamer | 500 | 3,000 | 81 | 366 |
| 8 | DBAL read model | 750 | 4,000 | 88 | 479 |
| 9 | Persistent SQLite connections | 1,000 | 4,000 | 132 | 466 |
| 10 | DBAL writes | 1,000 | 4,500 | 132 | 529 |
| 11 | No ORM | 1,500 | 4,500 | 177 | 539 |
| 12 | `CreatePostPayload::fromRequest()` | 1,500 | 4,500-4,750 | 183 | 590 |
| 13 | Prepared-statement reuse | 1,500 | 5,250 | 180 | 612 |
| 14 | SQLite writes the reads' JSON | 1,500 | 5,250 | 184 | 633 |
| 15 | The worker loop in `public/index.php` | 1,750 | 5,500 | 201 | 664 |

CPU stops every phase in both modes: at its limit the server is at the throttle's full quarter of a core, with at most
0.33 GiB of memory.

1. **Worker mode** (from phase 0): Symfony boots once per worker instead of once per request, ×3.1 users at phase 15.
   FrankenPHP's defaults are kept (2 workers, restarted every 500 requests): 1 worker, 4 workers and never
   restarting were within noise.
2. **The JIT** (phase 3): worker mode's long-lived workers keep the traces hot, +30% requests there, +3% in classic
   mode.
3. **A read model instead of serializing entities** (phases 6 and 8): the `Post` entities' `like_count` was an
   extra-lazy collection, so serializing the feed ran a COUNT query per post, and hydrating posts and authors cost
   more again. A DQL `NEW` expression (phase 6), then plain SQL through DBAL (phase 8), return the API's posts in one
   query.
4. **JsonStreamer instead of the Serializer** (phase 7): JsonStreamer generates the PHP code that writes each output
   class at cache warmup. Phase 14 then has SQLite write the reads' JSON itself.
5. **Persistent SQLite connections** (phase 9), classic mode's (+49% requests, worker mode already kept its
   connection): every request opened the database again, the pragmas read the schema, and closing the last
   connection made SQLite checkpoint and delete the WAL, so each write started over. A CPU profile didn't show it,
   a wall-clock one did.
6. **DBAL writes** (phase 10): one statement per write instead of the ORM's `find()`, `findOneBy()` and `flush()`
   (BEGIN, INSERT, COMMIT). **No ORM** (phase 11): without `doctrine/orm` and the entities, classic mode no longer
   boots it on every request (+35% requests there).
7. **Less idiomatic** (phases 12 to 15): `CreatePostPayload::fromRequest()` instead of `#[MapRequestPayload]` (no
   Serializer and Validator on the write path), prepared-statement reuse, SQLite writing the reads' JSON, and the
   worker loop of FrankenPHP's docs in `public/index.php` instead of `symfony/runtime`'s runner.
8. Smaller ones: php.ini production settings, preloading and the authoritative class map, every request to Symfony
   (`rewrite * /index.php` + `php` instead of `php_server`, no `try_files` filesystem checks), the firewall only on
   `POST /posts`.
9. Direct instead of Nginx: on one vCPU, Nginx's CPU would come out of the application's budget.

Tried and not kept, on phase 15 (requests served per second, against phase 15 in the same session; it moves by about
2% between rounds): `zend.enable_gc=0` (+7.2%, from one round of two, and a leaking cycle would grow a worker until
its restart), PR 40's Caddyfile settings (+3.7%), `GOGC=200` (+2.8%, 0.13 GiB more memory), `output_buffering=0`
(+1.5%), never restarting workers (+1.2%), SQLite's `cache_size` 64 MiB (+0.4%) and `mmap_size` 1 GiB (-3.1%),
`GOMEMLIMIT=1100MiB`, `GODEBUG=cgocheck=0`, 1 or 4 workers (within 0.5%), `max_threads 8` (-3.2%, -3.1% in classic
mode), 8 PHP threads all the time in classic mode (-0.7%). SQLite reads about as many pages per request (1.6-1.8
`pread64`) at every `cache_size` from 2 MiB (the default) to 64 MiB, likely because a connection drops its cache
when another one commits, which this load does every few milliseconds.

## On the leaderboard

The leaderboard's box scored this submission's first version (`48e3783`) at 15,500 users, CPU-bound (599 µs of CPU per
request), and PR 40, the other Symfony + FrankenPHP entry, at 21,000 (428 µs). Locally, at a full core, the server's
CPU sits near 50% at any load, which hides the gap; throttled to a quarter of the core, both saturate and PR 40 serves
35% more requests, as on the box. Hence the ladder's throttle.

Since then: FrankenPHP's release binary instead of the deb packages (the same FrankenPHP and PHP versions), no
`max_threads 8`, no ORM, the worker loop in `public/index.php` and `CreatePostPayload::fromRequest()`. In the same
throttled setup, the scored commit holds 3,750 users, phase 15 5,500 (+47%) and PR 40 5,750, one step ahead. At the
box's ratios for the scored commit and PR 40 (4.13 and 3.65 times their users here), phase 15 would score 20,000 to
22,700 there, a rough range.

## License

MIT, under the repo's [license](../../LICENSE).
