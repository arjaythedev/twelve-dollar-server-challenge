# PHP + Symfony (FrankenPHP worker mode)

A regular, idiomatic Symfony application, the kind you'd write at work, tuned through configuration and standard
Symfony components first. No custom HTTP server, no hand-written JSON, no group commit.

| | |
|---|---|
| Language | PHP 8.5.11 (ZTS), built into [FrankenPHP's release binary](https://github.com/php/frankenphp/releases/tag/v1.13.0) |
| Framework | Symfony 8.1 (FrameworkBundle 8.1.8, SecurityBundle, JsonStreamer, Runtime) |
| Server | FrankenPHP 1.13.0 (Caddy 2.11.7) in worker mode, through `symfony/runtime`'s FrankenPHP runner |
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
  reads never go through it. The JWT is checked on every request by an `AccessTokenHandlerInterface`:
  HS256 signature (`hash_hmac` + `hash_equals`), `exp` and, when present, `nbf`. It's hand-written because the
  challenge's 23-byte secret is shorter than RFC 7518's 256-bit minimum for HS256, which the maintained JWT libraries
  enforce.
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
| 0 | Idiomatic Symfony 8.1, FrankenPHP's release binary, defaults | 250 | 1,000 | 38 | 114 |
| 1 | php.ini production settings (OPcache, realpath) | 250 | 1,000 | 39 | 116 |
| 2 | Preloading, authoritative class map | 250 | 1,000 | 47 | 120 |
| 3 | JIT (tracing) | 250 | 1,250 | 50 | 155 |
| 4 | DQL read model | 500 | 2,250 | 63 | 244 |
| 5 | JsonStreamer | 500 | 3,000 | 74 | 342 |
| 6 | DBAL read model | 750 | 4,000 | 85 | 422 |
| 7 | Persistent SQLite connections | 1,000 | 4,000 | 120 | 429 |
| 8 | DBAL writes | 1,000 | 4,500 | 122 | 470 |
| 9 | No ORM | 1,500 | 4,500 | 156 | 482 |
| 10 | `CreatePostPayload::fromRequest()` | 1,500 | 4,500-4,750 | 162 | 513 |
| 11 | Prepared-statement reuse | 1,500 | 5,250 | 153 | 569 |
| 12 | SQLite writes the reads' JSON | 1,500 | 5,250 | 160 | 592 |
| 13 | Every request to Symfony (`rewrite` + `php`) | 1,500 | 5,250 | 166 | 630 |
| 14 | Firewall on `POST /posts` only | 1,500 | 5,250 | 178 | 648 |

The users were searched in an earlier order of the phases, where phases 13 and 14 came right after the JIT: phases 4 to
12 serve 3% to 15% fewer requests without them, likely one to three steps of users fewer in worker mode
([`LADDER.md`](local-bench/LADDER.md)).

CPU stops every phase in both modes: at its limit the server is at the throttle's full quarter of a core, with at most
0.32 GiB of memory.

1. **Worker mode** (from phase 0): Symfony boots once per worker instead of once per request, ×3.5 users at phase 14.
   FrankenPHP's defaults are kept (2 workers, restarted every 500 requests): 1 worker (+4.1%, at the edge of the
   noise), 4 workers and never restarting weren't above it.
2. **The JIT** (phase 3): worker mode's long-lived workers keep the traces hot, +30% requests there, +7% in classic
   mode.
3. **A read model instead of serializing entities** (phases 4 and 6): the `Post` entities' `like_count` was an
   extra-lazy collection, so serializing the feed ran a COUNT query per post, and hydrating posts and authors cost
   more again. A DQL `NEW` expression (phase 4), then plain SQL through DBAL (phase 6), return the API's posts in one
   query.
4. **JsonStreamer instead of the Serializer** (phase 5): JsonStreamer generates the PHP code that writes each output
   class at cache warmup. Phase 12 then has SQLite write the reads' JSON itself.
5. **Persistent SQLite connections** (phase 7), classic mode's (+41% requests, worker mode already kept its
   connection): every request opened the database again, the pragmas read the schema, and closing the last
   connection made SQLite checkpoint and delete the WAL, so each write started over. A CPU profile didn't show it,
   a wall-clock one did.
6. **DBAL writes** (phase 8): one statement per write instead of the ORM's `find()`, `findOneBy()` and `flush()`
   (BEGIN, INSERT, COMMIT). **No ORM** (phase 9): without `doctrine/orm` and the entities, classic mode no longer
   boots it on every request (+29% requests there).
7. **Less idiomatic** (phases 10 to 12): `CreatePostPayload::fromRequest()` instead of `#[MapRequestPayload]` (no
   Serializer and Validator on the write path), prepared-statement reuse and SQLite writing the reads' JSON.
8. **Every request to Symfony** (phase 13): `rewrite * /index.php` + `php` instead of `php_server`, no `try_files`
   filesystem checks. +1% when it came right after the JIT, +6% after phase 12, once requests are cheap.
9. **The firewall on `POST /posts` only** (phase 14): reads skip it. Within noise in worker mode (+2.9%), +7.3% in
   classic mode, where Symfony boots on every request.
10. Smaller ones: php.ini production settings, preloading and the authoritative class map.
11. Direct instead of Nginx: on one vCPU, Nginx's CPU would come out of the application's budget.

Tried and not kept, on phase 14's app, as the last phase of the earlier order (requests served per second, against it in
the same session; an experiment's two rounds differ by up to 8%): `zend.enable_gc=0` (+8.8%, in both rounds, and a
leaking cycle would grow a worker until its restart), 1 worker (+4.1%), `GOMEMLIMIT=1100MiB` (+3.6%), FrankenPHP's
worker loop without `symfony/runtime` (+2.5%, and -2.5% in another session), SQLite's `cache_size` 64 MiB (+2.1%) and
`mmap_size` 1 GiB (-2.1%), PR 40's Caddyfile settings (+2.0%), never restarting workers (+1.1%), `GODEBUG=cgocheck=0`,
`output_buffering=0`, `GOGC=200` (within 0.5%, 0.2 GiB more memory for `GOGC`), 4 workers (-2.6%), `max_threads 8`
(-3.5%, -2.1% in classic mode), 8 PHP threads all the time in classic mode (-2.5%). SQLite reads about as many pages per
request (1.6-1.8 `pread64`) at every `cache_size` from 2 MiB (the default) to 64 MiB, likely because a connection drops
its cache when another one commits, which this load does every few milliseconds.

An earlier phase 15 had its own worker loop instead of `symfony/runtime`'s runner, with `symfony/runtime` still
installed: FrameworkBundle then leaves registering Symfony's `ErrorHandler` to `symfony/runtime`, so the loop ran
without one, which was its gain (+2.7%). With `symfony/runtime` removed, the same loop is within noise in worker mode,
and phase 15 was dropped.

## On the leaderboard

The leaderboard's box scored this submission's first version (`48e3783`) at 15,500 users, CPU-bound (599 µs of CPU per
request), and PR 40, the other Symfony + FrankenPHP entry, at 21,000 (428 µs). Locally, at a full core, the server's
CPU sits near 50% at any load, which hides the gap; throttled to a quarter of the core, both saturate and PR 40 serves
35% more requests, as on the box. Hence the ladder's throttle.

Since then: FrankenPHP's release binary instead of the deb packages (the same FrankenPHP and PHP versions), no
`max_threads 8`, no ORM and `CreatePostPayload::fromRequest()`. In the same throttled setup, the scored commit holds
3,750 users, phase 14 5,250 (+40%) and PR 40 5,750, two steps ahead. #256 (`4a11a5d`), an earlier phase 15 with its
own worker loop, then scored 18,500 on the box, and 5,500 users here: at its ratio (3.36), the closest of the three,
phase 14 would score about 17,700 there, and 19,200 to 21,700 at the scored commit's and PR 40's (4.13 and 3.65), a
rough estimate.

## License

MIT, under the repo's [license](../../LICENSE).
