# Profiling phase 7

Phase 7 (DBAL read model) measured after the fact, against phase 6 (PROFILE-p6.md). The question: phase 6's profile
had the ORM at 38% of the feed and 44% of `GET /posts/{id}` in worker mode. Does plain SQL take it all away? Measured
2026-10-21, same setup as LADDER.md.

**In short**: yes, the ORM's part of the reads goes (from 78,569 samples to under 500), for a little DBAL and the
mapping of rows to `PostView`. The feed costs 32% fewer samples, `GET /posts/{id}` 37%, and a k6 user's requests 13%
less CPU in worker mode, 6% in classic mode. After it, in worker mode, the controllers are under a third of PHP's
samples: the rest is Symfony around them (events, routing, the firewall, the request and the response).

## CPU, under load

The same users as PROFILE-p5.md and PROFILE-p6.md (classic mode at 5,000, worker mode at 15,000), under the real load
mix of `bench/load.js`. Without warm-up, 30 s ramp-up, 2 min hold.

```sh
for name in p6 p7; do
  commit="$(git log --format=%h -1 --grep="^php-symfony-gnugat: phase ${name#p},")"
  git worktree add "$TMPDIR/$name" "$commit"
  IMAGE="twelve-dollar-php-symfony:$name" bash "$TMPDIR/$name/submissions/php-symfony-gnugat/local-bench/bench.sh" build
  docker build --build-arg BASE="twelve-dollar-php-symfony:$name" -f local-bench/profile/Dockerfile \
    -t "twelve-dollar-php-symfony-profile:$name" local-bench/profile
done
# for each phase (p6, p7) and mode (profile/Caddyfile at 5,000 users, profile/Caddyfile.worker at 15,000)
IMAGE=twelve-dollar-php-symfony-profile:p6 \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 15000 p6-worker
mkdir p6-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p6-worker/
cp local-bench/results/*-p6-worker-15000.k6 p6-worker/k6.txt
python3 -I local-bench/profile/compare.py p6-worker p7-worker PostController::feed
python3 -I local-bench/profile/analyze.py p7-worker HttpKernel::handle PostController:: EventDispatcher::dispatch
```

|            |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | PHP samples |
|------------|-------:|------------:|----------:|-------:|---------:|---------:|------------:|
| Classic p6 |  5,000 |     0.008 s |   0.013 s |  0.00% |      50% |   83,591 |     117,447 |
| Classic p7 |  5,000 |     0.009 s |   0.014 s |  0.00% |      52% |   83,511 |     128,040 |
| Worker  p6 | 15,000 |     0.005 s |   0.009 s |  0.00% |      60% |  250,831 |     714,574 |
| Worker  p7 | 15,000 |     0.005 s |   0.009 s |  0.00% |      57% |  250,585 |     647,487 |

`analyze.py` reads every `.txt` file in the directory: give it one with only `stacks.txt` in it.

### Worker mode, 15,000 users

| Share of the frame | Feed p6 | Feed p7 | Post p6 | Post p7 | `handle` p6 | `handle` p7 |
|---|--:|--:|--:|--:|--:|--:|
| SQLite: PDO prepare, execute, fetch | 37.7% | 53.1% | 33.9% | 55.6% | 26.8% | 30.7% |
| ORM: hydration, DQL to SQL, query building | 38.3% | 0.3% | 44.2% | 0.5% | 28.7% | 9.0% |
| DBAL, besides SQLite (parameters, results) | | 1.1% | | 2.0% | | 0.6% |
| JsonStreamer (and its generated writers) | 17.1% | 28.6% | 7.7% | 14.6% | 7.9% | 10.4% |
| `JsonResponse` | 6.2% | 9.9% | 12.2% | 20.1% | 5.0% | 6.2% |
| Controller, other (`PostView::fromRow()` among them) | 0.8% | 7.0% | 2.0% | 7.3% | 31.6% | 43.1% |
| **Samples per 1,000 requests** | **515** | **349 (-32%)** | **264** | **166 (-37%)** | **1,447** | **1,238 (-14%)** |

Feed is `PostController::feed`, post `PostController::show` (`GET /posts/{id}`), `handle` is `HttpKernel::handle`.

- the ORM's part of the reads, 49,384 samples in the feed and 29,185 in `GET /posts/{id}`, is 306 and 192 after;
  DBAL adds 938 and 840, and "other" 5,047 and 1,646, where the mapping of rows to `PostView` (our code) falls;
- SQLite's part stays the same (48,643 -> 46,487 in the feed, 22,441 -> 23,179 in the post): the same query, now
  written in SQL;
- what's left of the ORM under `HttpKernel::handle` (9%) is the writes, which still go through it.

After phase 7, of all of PHP's samples:

| Share of PHP samples, worker mode p7 | |
|---|--:|
| `HttpKernel::handle` (the rest: FrankenPHP's request loop, response sending, worker boot) | 47.9% |
| `PostController` (the four endpoints) | 28.9% |
| `EventDispatcher::dispatch` (kernel events) | 16.3% |
| of which the router (`RouterListener`) | 4.6% |
| of which the firewall | 4.2% |
| `Request` | 5.5% |
| `Response::send` | 5.4% |

### Classic mode, 5,000 users

The profile has the reads cheaper (feed -14% per request) but the whole a little dearer (+9% PHP samples), where
`/proc` below has every request cheaper or the same: classic mode's CPU profile undercounts (each request starts its
own timer, and the kernel only checks it on its tick, about every 1 ms), so its sizes aren't usable. And classic
mode's limit isn't the CPU anyway: at its limit it fails at 80-85% CPU (phase 7's commit), so something waits, and a
CPU profile doesn't see waits.

## CPU, per endpoint

Without a profiler: FrankenPHP's CPU in `/proc` (all its threads, Go included) for 1,000 sequential requests of each
kind, after 300 to warm up (`profile/percpu.sh`, worker mode with the phase's `local-bench/Caddyfile.worker`).

| ms of CPU per request | Worker p6 | Worker p7 | Classic p6 | Classic p7 |
|---|--:|--:|--:|--:|
| `GET /feed` | 0.25 | 0.21 (-16%) | 0.68 | 0.63 (-7%) |
| `GET /posts/{id}` | 0.21 | 0.19 (-10%) | 0.66 | 0.57 (-14%) |
| `POST /posts/{id}/like` | 0.31 | 0.30 | 1.64 | 1.49 |
| `POST /posts` | 0.34 | 0.33 | 1.88 | 1.83 |
| `mix` (feed, post, a like one time in 7) | 0.48 | 0.42 (-13%) | 1.47 | 1.38 (-6%) |

`/proc` counts in 10 ms, so ±0.01 ms per request. Worker mode's hold CPU at 15,000 users: 60% to 57%.

## Next

- Worker mode: Symfony's work around the controllers, the kernel events above all (16% of PHP's samples, the router
  and the firewall 4-5% each); the firewall runs on every request, where only the writes need it.
- Classic mode: what it waits on. A wall-clock profile would show it, where this CPU one can't.
