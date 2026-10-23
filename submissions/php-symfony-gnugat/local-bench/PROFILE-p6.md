# Profiling phase 6

Phase 6 (JsonStreamer) measured after the fact, against phase 5 (PROFILE-p5.md). The question: phase 5's profile had
the Serializer at 64% of the feed in worker mode. How much of it does JsonStreamer take away? Measured 2026-10-21, same
setup as LADDER.md.

**In short**: four fifths of it. Writing the feed's JSON goes from 157,314 samples to 30,143 (-81%), and the feed
costs 45% fewer samples, 28% less CPU in `/proc`; a k6 user's requests 24% less CPU in worker mode, 19% in classic
mode. After it, the feed is the query: SQLite 38%, the ORM's DQL, hydration and query building 38% between them.

## CPU, under load

The same users as PROFILE-p5.md (classic mode at 5,000, worker mode at 15,000), under the real load mix of
`bench/load.js`. Without warm-up, 30 s ramp-up, 2 min hold.

```sh
for name in p5 p6; do
  commit="$(git log --format=%h -1 --grep="^php-symfony-gnugat: phase ${name#p},")"
  git worktree add "$TMPDIR/$name" "$commit"
  IMAGE="twelve-dollar-php-symfony:$name" bash "$TMPDIR/$name/submissions/php-symfony-gnugat/local-bench/bench.sh" build
  docker build --build-arg BASE="twelve-dollar-php-symfony:$name" -f local-bench/profile/Dockerfile \
    -t "twelve-dollar-php-symfony-profile:$name" local-bench/profile
done
# for each phase (p5, p6) and mode (profile/Caddyfile at 5,000 users, profile/Caddyfile.worker at 15,000)
IMAGE=twelve-dollar-php-symfony-profile:p5 \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 15000 p5-worker
mkdir p5-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p5-worker/
cp local-bench/results/*-p5-worker-15000.k6 p5-worker/k6.txt
python3 -I local-bench/profile/compare.py p5-worker p6-worker PostController::feed
```

|            |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | PHP samples |
|------------|-------:|------------:|----------:|-------:|---------:|---------:|------------:|
| Classic p5 |  5,000 |     0.008 s |   0.012 s |  0.00% |      50% |   83,468 |     116,098 |
| Classic p6 |  5,000 |     0.008 s |   0.013 s |  0.00% |      50% |   83,591 |     117,447 |
| Worker  p5 | 15,000 |     0.004 s |   0.008 s |  0.00% |      60% |  250,406 |     771,464 |
| Worker  p6 | 15,000 |     0.005 s |   0.009 s |  0.00% |      60% |  250,831 |     714,574 |

`compare.py` puts each sample under a frame in the first bucket it matches, in its order: a sample under
`Connection::connect` is the connection's, then a sample ending in PDO is SQLite's, even inside hydration.
JsonStreamer's bucket takes its generated writers (in `var/cache/prod/json_streamer/`) and the anonymous iterator
`JsonStreamWriter` returns, besides its own classes.

### Worker mode, 15,000 users

| Share of the frame | Feed p5 | Feed p6 | Post p5 | Post p6 | `handle` p5 | `handle` p6 |
|---|--:|--:|--:|--:|--:|--:|
| SQLite: PDO prepare, execute, fetch | 15.7% | 37.7% | 30.0% | 33.9% | 17.5% | 26.8% |
| ORM hydration | 9.6% | 23.1% | 9.4% | 9.5% | 7.7% | 11.2% |
| ORM DQL to SQL (`Query::getSqlExecutor`, query cache) | 3.0% | 7.0% | 14.0% | 16.5% | 3.7% | 5.5% |
| ORM, besides SQLite (query building, execute) | 3.7% | 8.2% | 15.7% | 18.2% | 8.4% | 12.0% |
| Serializer (normalize) | 64.0% | | 19.1% | | 38.4% | 0.7% |
| JsonStreamer (and its generated writers) | | 17.1% | | 7.7% | | 7.9% |
| `JsonResponse` | 3.1% | 6.2% | 9.8% | 12.2% | 3.5% | 5.0% |
| Controller, other | 0.7% | 0.8% | 2.0% | 2.0% | 20.7% | 31.0% |
| **Samples per 1,000 requests** | **936** | **515 (-45%)** | **255** | **264 (+4%)** | **1,714** | **1,447 (-16%)** |

Feed is `PostController::feed`, post `PostController::show` (`GET /posts/{id}`), `handle` is `HttpKernel::handle`.

- the feed: the Serializer and `JsonResponse` were 157,314 samples, JsonStreamer and `JsonResponse` are 30,143. The
  Serializer walked 20 `PostView` objects through `ObjectNormalizer` (property access, metadata, attributes, on every
  request); JsonStreamer's generated code writes their properties straight to the output;
- `GET /posts/{id}`: its JSON goes from 18,436 samples to 13,217 (-28%: one post, so `JsonResponse` and the writer's
  set-up weigh more), but the endpoint as a whole doesn't gain here (+4%), the query's part growing by as much, with
  the same code. Not explained; `/proc` below has it 5% cheaper.

### Classic mode, 5,000 users

The Serializer was 31.6% of the feed's samples and 19.3% of `GET /posts/{id}`'s; both go. But classic mode's CPU
profile undercounts (each request starts its own timer, and the kernel only checks it on its tick, about every 1 ms),
and the connection opened per request is 29-56% of the reads' samples: `/proc` below has the sizes.

## CPU, per endpoint

Without a profiler: FrankenPHP's CPU in `/proc` (all its threads, Go included) for 1,000 sequential requests of each
kind, after 300 to warm up. `mix` is a k6 user's loop: the feed, a post, a like one time in 7.

```sh
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p5                # classic mode
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p5 \
  "$TMPDIR/p5/submissions/php-symfony-gnugat/local-bench/Caddyfile.worker"     # worker mode
```

| ms of CPU per request | Worker p5 | Worker p6 | Classic p5 | Classic p6 |
|---|--:|--:|--:|--:|
| `GET /feed` | 0.34-0.35 | 0.25 (-28%) | 0.90 | 0.68 (-24%) |
| `GET /posts/{id}` | 0.22 | 0.21 (-5%) | 0.76 | 0.66 (-13%) |
| `POST /posts/{id}/like` | 0.30 | 0.31 | 1.55 | 1.64 |
| `POST /posts` | | 0.34 | | 1.88 |
| `mix` | 0.62-0.65 | 0.48 (-24%) | 1.82 | 1.47 (-19%) |

`/proc` counts in 10 ms, so ±0.01 ms per request; phase 5 is from PROFILE-p5.md, without `POST /posts`.

Worker mode's hold CPU under load barely moves (60%), as in PROFILE-p5.md, where PHP's samples go down 7% and `/proc`
24%: Go's work (connections, keep-alives) stays the same.

## Next

- The feed is now its query: SQLite 38%, the ORM 38% (hydration 23%, DQL 7%, query building 8%). For
  `GET /posts/{id}`, the ORM is 44%. A DBAL read model (plain SQL, rows mapped by hand) keeps SQLite's part and drops
  the ORM's.
