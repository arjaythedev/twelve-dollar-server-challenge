# Profiling phase 4

Where the CPU goes after phase 4 (tracing JIT), measured after the fact, to see what a profile would have pointed at
before phase 5. Measured 2026-10-21, same setup as LADDER.md.

**In short**: the feed. In worker mode it's 44% of PHP's samples, and in it the extra-lazy `like_count` (a COUNT query
per post, 20 per request) is 37%, the Serializer 35%, the ORM's hydration of each `Post` and its `User` 16%. The SQL
of the feed itself is 7%.

## The profiler

Excimer samples the PHP threads' CPU clock (see `profile/excimer.php`). Checked in FrankenPHP: 2 s of spinning in a
request gives 2.0 s of samples, alone or next to another busy thread, and a sleeping request gets none (the clock is
the thread's). Against FrankenPHP's own CPU in `/proc` (all its threads, Go included), 1,200 sequential `GET /feed`
in worker mode: 397 ms of PHP samples for 500 ms of process CPU.

But the kernel only checks the timer on its tick, about every 1 ms here: the 0.1 ms period gives ~10 events per tick,
not 10 times the resolution. That matters in classic mode (below).

## CPU, under load

Under the real load mix of `bench/load.js` (per user loop: 2 reads, a like 15% of the time and a post 2% of the time),
below this phase's limits (classic 11,000, worker 33,000). Without warm-up, 30 s ramp-up, 2 min hold.

```sh
IMAGE=twelve-dollar-php-symfony:p4 bash local-bench/bench.sh build
docker build --build-arg BASE=twelve-dollar-php-symfony:p4 -f local-bench/profile/Dockerfile \
  -t twelve-dollar-php-symfony-profile:p4 local-bench/profile
# for each mode (profile/Caddyfile at 5,000 users, profile/Caddyfile.worker at 15,000)
IMAGE=twelve-dollar-php-symfony-profile:p4 \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 15000 p4-worker
mkdir p4-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p4-worker/
python3 -I local-bench/profile/analyze.py p4-worker HttpKernel::handle PostController:: EventDispatcher::dispatch
cp local-bench/results/*-p4-worker-15000.k6 p4-worker/k6.txt
python3 -I local-bench/profile/compare.py p4-worker p4-worker PostController::feed   # one run: both columns alike
```

|            |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | Iterations | PHP samples |
|------------|-------:|------------:|----------:|-------:|---------:|---------:|-----------:|------------:|
| Classic p4 |  5,000 |     0.008 s |   0.013 s |  0.00% |      53% |   83,689 |     36,001 |     110,893 |
| Worker  p4 | 15,000 |     0.004 s |   0.007 s |  0.00% |      62% |  250,697 |    107,883 |     857,847 |

`analyze.py` reads every `.txt` file in the directory: run it before adding `k6.txt`, or give it a directory with
only `stacks.txt` in it.

### Worker mode, 15,000 users

| Share of PHP samples | |
|---|--:|
| `HttpKernel::handle` | 62.8% |
| `GET /feed` | 44.3% |
| `GET /posts/{id}` | 7.8% |
| `POST /posts/{id}/like` | 2.7% |
| `POST /posts` | 0.4% |
| `EventDispatcher::dispatch` (kernel events) | 6.5% |

Outside `HttpKernel::handle`: 27.8% of the samples are self time of the worker's request loop (the closure in
`FrankenPhpWorkerRunner`), most likely FrankenPHP's C work between requests on the PHP thread, charged to the PHP frame
it runs under; and `Response::send`, 3.1%.

`compare.py` puts each sample under a frame in the first bucket it matches, in this order: a sample under
`getLikeCount()` is the COUNT's, even when it ends in PDO; then a sample ending in PDO is SQLite's, even inside
hydration.

| Share of the frame | Feed | Post | `handle` |
|---|--:|--:|--:|
| `like_count`: a COUNT query per post (extra lazy) | 36.6% | 13.5% | 27.5% |
| SQLite: PDO prepare, execute, fetch | 6.9% | 19.1% | 9.5% |
| ORM hydration | 16.3% | 27.0% | 15.2% |
| ORM DQL to SQL (`Query::getSqlExecutor`, query cache) | 1.6% | | 1.1% |
| ORM query building, `find()`, execute besides SQLite | 1.7% | 16.1% | 5.0% |
| Serializer (normalize) | 34.9% | 15.9% | 26.9% |
| JSON response | 1.5% | 6.2% | 2.0% |
| Controller, other | 0.4% | 2.2% | 12.6% |
| **Samples per 1,000 requests** | **1,515** | **266** | **2,149** |

Feed is `PostController::feed`, post `PostController::show` (`GET /posts/{id}`), `handle` is `HttpKernel::handle`.

- the feed: `Post::getLikeCount()` counts an extra-lazy collection, so the Serializer runs one COUNT query per post,
  20 per request, through the ORM's collection persister: 17.3% of all PHP samples. Hydrating each `Post` and its
  `User` (a lazy proxy, loaded for `getAuthorUsername()`) is another 16%;
- the Serializer walks every `Post` through `ObjectNormalizer`: `PropertyAccessor::getValue` alone is 19.8% of all PHP
  samples, reading the attributes' metadata and context 8%;
- `GET /posts/{id}`: one post, loaded with `find()`; hydration and the ORM's persister are 43% of it.

### Classic mode, 5,000 users

The same direction for the feed (the COUNTs 29% of it, hydration 10%), with SQLite larger everywhere (a connection
opened per request). But don't read classic mode's absolute counts: its 110,893 samples are 11 s of CPU for 83,689
requests, 0.13 ms each, against about 1.2 ms per request in `/proc` (below, `mix`). Each request starts its own
timer, and whatever CPU it uses after the kernel's last tick is never sampled, a large part of a 1-2 ms request.

## CPU, per endpoint

Without a profiler: FrankenPHP's CPU in `/proc` (all its threads, Go included) for 1,000 sequential requests of each
kind, after 300 to warm up. `mix` is a k6 user's loop: the feed, a post, a like one time in 7.

```sh
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p4                               # classic mode
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p4 "$PWD/local-bench/Caddyfile.worker"  # worker mode
```

| ms of CPU per request | Worker | Classic |
|---|--:|--:|
| `GET /feed` | 0.56-0.60 | 1.20 |
| `GET /posts/{id}` | 0.25-0.28 | 0.85 |
| `POST /posts/{id}/like` | 0.30-0.34 | 1.57 |
| `mix` | 0.89-0.93 | 2.64 |

Worker mode twice, classic mode once; `/proc` counts in 10 ms, so ±0.01 ms per request.

## Next

- The COUNT per post and the hydration: a read model that selects the feed's columns, with the like count as a
  subquery, in one query, without managed entities.
- Then the Serializer, 35% of the feed.
