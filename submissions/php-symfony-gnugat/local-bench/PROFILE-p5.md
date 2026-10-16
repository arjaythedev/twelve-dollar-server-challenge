# Profiling phase 5

Phase 5 (DQL read model) measured after the fact, against phase 4 (PROFILE-p4.md). The question: PROFILE-p4.md has
the extra-lazy `like_count` (a COUNT query per post) at 37% of the feed in worker mode, and hydration another 16%. Does
the read model take them away, and where does the profile point next? Measured 2026-10-21, same setup as LADDER.md.

**In short**: yes. Phase 5 removes the first and halves the second: the feed costs 40% less CPU, a k6 user's requests
30% less, in both modes. After it, the Serializer is 64% of the feed, and the DQL that replaced `find()` is 14% of
`GET /posts/{id}`.

## CPU, under load

Both phases with the same users, under the real load mix of `bench/load.js` (per user loop: 2 reads, a like 15% of
the time and a post 2% of the time), below phase 4's limits (classic 11,000, worker 33,000). Without warm-up, 30 s
ramp-up, 2 min hold.

```sh
for name in p4 p5; do
  commit="$(git log --format=%h -1 --grep="^php-symfony-gnugat: phase ${name#p},")"
  git worktree add "$TMPDIR/$name" "$commit"
  IMAGE="twelve-dollar-php-symfony:$name" bash "$TMPDIR/$name/submissions/php-symfony-gnugat/local-bench/bench.sh" build
  docker build --build-arg BASE="twelve-dollar-php-symfony:$name" -f local-bench/profile/Dockerfile \
    -t "twelve-dollar-php-symfony-profile:$name" local-bench/profile
done
# for each phase (p4, p5) and mode (profile/Caddyfile at 5,000 users, profile/Caddyfile.worker at 15,000)
IMAGE=twelve-dollar-php-symfony-profile:p4 \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 15000 p4-worker
mkdir p4-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p4-worker/
cp local-bench/results/*-p4-worker-15000.k6 p4-worker/k6.txt
python3 -I local-bench/profile/compare.py p4-worker p5-worker PostController::feed
python3 -I local-bench/profile/delta.py p4-worker p5-worker PostController::like
```

|            |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | Iterations | PHP samples |
|------------|-------:|------------:|----------:|-------:|---------:|---------:|-----------:|------------:|
| Classic p4 |  5,000 |     0.008 s |   0.013 s |  0.00% |      53% |   83,689 |     36,001 |     110,893 |
| Classic p5 |  5,000 |     0.008 s |   0.012 s |  0.00% |      50% |   83,468 |     35,899 |     116,098 |
| Worker  p4 | 15,000 |     0.004 s |   0.007 s |  0.00% |      62% |  250,697 |    107,883 |     857,847 |
| Worker  p5 | 15,000 |     0.004 s |   0.008 s |  0.00% |      60% |  250,406 |    107,789 |     771,464 |

`compare.py` puts each sample under a frame in the first bucket it matches, in this order: a sample under
`getLikeCount()` is the COUNT's, even when it ends in PDO; then a sample ending in PDO is SQLite's, even inside
hydration.

### Worker mode, 15,000 users

| Share of the frame | Feed p4 | Feed p5 | Post p4 | Post p5 | `handle` p4 | `handle` p5 |
|---|--:|--:|--:|--:|--:|--:|
| `like_count`: a COUNT query per post (extra lazy) | 36.6% | | 13.5% | | 27.5% | |
| SQLite: PDO prepare, execute, fetch | 6.9% | 15.7% | 19.1% | 30.0% | 9.5% | 17.6% |
| ORM hydration | 16.3% | 9.6% | 27.0% | 9.4% | 15.2% | 7.7% |
| ORM DQL to SQL (`Query::getSqlExecutor`, query cache) | 1.6% | 3.0% | | 14.0% | 1.1% | 3.7% |
| ORM query building, execute besides SQLite | 1.7% | 3.7% | 16.1% | 15.7% | 5.0% | 8.4% |
| Serializer (normalize) | 34.9% | 64.0% | 15.9% | 19.1% | 26.9% | 38.4% |
| JSON response | 1.5% | 3.1% | 6.2% | 9.8% | 2.0% | 3.5% |
| Controller, other | 0.4% | 0.7% | 2.2% | 2.0% | 12.6% | 20.7% |
| **Samples per 1,000 requests** | **1,515** | **936 (-38%)** | **266** | **255 (-4%)** | **2,149** | **1,714 (-20%)** |

Feed is `PostController::feed`, post `PostController::show` (`GET /posts/{id}`), `handle` is `HttpKernel::handle`.

- the feed: the COUNTs go (one query per post, 20 per request), hydration more than halves (a `NEW PostView` row
  instead of a `Post` and its `User`), SQLite grows (the correlated subquery counts the likes inside the one query);
- `GET /posts/{id}` barely gains: phase 4 loaded the post with `find()`, which skips DQL. Phase 5's query builder and
  its DQL-to-SQL step (a query cache lookup, every request) cost 14% of the endpoint, about what the COUNT and the
  hydration saved;
- after phase 5, the Serializer is 64% of the feed and `ObjectNormalizer` 20% of all PHP samples: the next target.

### Classic mode, 5,000 users

Same direction for the feed (-26% samples per request: the COUNTs were 29% of it, hydration 10%), with SQLite larger
everywhere (the connection opened per request). But don't read classic mode's absolute counts: its 110,893 samples are
11 s of CPU for 83,689 requests, 0.13 ms each, against about 1 ms per request in `/proc` (below, `mix`). Each request
starts its own timer, and whatever CPU it uses after the kernel's last tick is never sampled, a large part of a
1-2 ms request. The shares lean away from the ends of requests, and PHP samples went up 5% while the server's hold CPU
went down 6%.

## CPU, per endpoint

Without a profiler: FrankenPHP's CPU in `/proc` (all its threads, Go included) for 1,000 sequential requests of each
kind, after 300 to warm up. `mix` is a k6 user's loop: the feed, a post, a like one time in 7.

```sh
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p4                # classic mode
bash local-bench/profile/percpu.sh twelve-dollar-php-symfony:p4 \
  "$TMPDIR/p4/submissions/php-symfony-gnugat/local-bench/Caddyfile.worker"     # worker mode
```

| ms of CPU per request | Worker p4 | Worker p5 | Classic p4 | Classic p5 |
|---|--:|--:|--:|--:|
| `GET /feed` | 0.56-0.60 | 0.34-0.35 (-40%) | 1.20 | 0.90 (-25%) |
| `GET /posts/{id}` | 0.25-0.28 | 0.22 (-17%) | 0.85 | 0.76 (-11%) |
| `POST /posts/{id}/like` | 0.30-0.34 | 0.30 | 1.57 | 1.55 |
| `mix` | 0.89-0.93 | 0.62-0.65 (-30%) | 2.64 | 1.82 (-31%) |

Worker mode twice, classic mode once; `/proc` counts in 10 ms, so ±0.01 ms per request.

Worker mode's hold CPU under load only went from 62% to 60%, where both this table and Excimer say PHP's share of
the work dropped by a fifth to a third. The ladder's runs saw the same (-5% CPU at a fixed load). Not explained: hold
CPU is a median of `docker stats` samples, and Go's share (connections, the k6 users' keep-alives) doesn't change.

## Likes: not explained

Phase 5 doesn't touch the like, and alone it costs the same in both phases (table above). Yet under load it has more
samples in phase 5, in both modes and in two runs each:

| Under `PostController::like` | p4 | p5 | |
|---|--:|--:|--:|
| Worker mode, all of it | 22,857 | 39,667 | +74% |
| `find()` (the post) | 4,260 | 8,898 | +109% |
| `findOneBy()` (the like) | 3,499 | 7,509 | +115% |
| `flush()` | 12,285 | 19,179 | +56% |
| Classic mode, all of it | 35,919 | 47,083 | +31% |
| `PDO\Connection::commit` | 28,186 | 36,691 | +30% |

About the same number of likes in both runs (the same users, iterations within 0.3%), so each one costs more, but only
next to the reads. Guesses, not measured:

- worker mode, where all of the like's Doctrine work grows: phase 4's feed ran 20 COUNT queries through DBAL per
  request, and `GET /posts/{id}` used `find()`, which kept that code hot (JIT traces, CPU caches); in phase 5, the
  writes are the only ones left on `find()`, `findOneBy()` and the persisters, about 7% of the requests;
- classic mode, where it's the commit: SQLite's own C code, so not the JIT. Perhaps the WAL: a checkpoint can't go
  past what a reader still uses, and phase 5's reads are one longer query instead of 21 short ones.

## Next

- The Serializer: 64% of the feed and 19% of `GET /posts/{id}` in worker mode, 38% of `HttpKernel::handle`.
  JsonStreamer writes the JSON without normalizing.
- The DQL step: 14% of `GET /posts/{id}`, SQL built from DQL on every request. A DBAL read model (plain SQL) skips the
  query builder, the query cache lookup and the hydration.
