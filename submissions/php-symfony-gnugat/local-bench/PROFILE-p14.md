# Profiling phase 14

Phase 14 (DBAL writes) measured after the fact, against phase 13. The question: PROFILE-p12.md put Doctrine's own
work in the writes at 4.8% of PHP's CPU in worker mode, the most DBAL writes could save. Did they? Measured 2026-10-21,
same setup as LADDER.md.

**In short**: yes, and a little more. In phase 13, Doctrine's work in the writes is 4.7% of PHP's samples in worker
mode; phase 14 removes almost all of it, and SQLite's share shrinks too, a like being one statement instead of five. A
like costs 58% fewer samples, a post 34% fewer: 5.3% of PHP's samples. In `/proc`, a like goes from 0.27 to 0.20 ms
of CPU in worker mode, 0.71 to 0.52 ms in classic mode. The reads don't change, and they're most of the requests: the
server's CPU at a fixed load stays the same. Worker mode's gain on the ladder (77,500 -> 79,000) is its memory.

## CPU, under load

Both phases with the same users, under the real load mix of `bench/load.js`, the users of PROFILE-p12.md: worker mode
at 30,000, classic mode at 8,000. Without warm-up, 30 s ramp-up, 2 min hold.

```sh
p13="$(git log --format=%h -1 --grep='^php-symfony-gnugat: phase 13,')"
git worktree add "$TMPDIR/p13" "$p13"
IMAGE=twelve-dollar-php-symfony:p13 bash "$TMPDIR/p13/submissions/php-symfony-gnugat/local-bench/bench.sh" build
docker build --build-arg BASE=twelve-dollar-php-symfony:p13 -f local-bench/profile/Dockerfile \
  -t twelve-dollar-php-symfony-profile:p13 local-bench/profile
# the same for this phase (p14), from this checkout; then for each phase and mode (profile/Caddyfile.worker at
# 30,000 users, profile/Caddyfile at 8,000):
IMAGE=twelve-dollar-php-symfony-profile:p13 \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 30000 p13-worker
mkdir p13-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p13-worker/
cp local-bench/results/*-p13-worker-30000.k6 p13-worker/k6.txt
python3 -I local-bench/profile/compare.py p13-worker p14-worker PostController::like
python3 -I local-bench/profile/delta.py p13-worker p14-worker PostController::like
```

|             |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | PHP samples |
|-------------|-------:|------------:|----------:|-------:|---------:|---------:|------------:|
| Classic p13 |  8,000 |     0.005 s |   0.008 s |  0.00% |      57% |  133,511 |      44,752 |
| Classic p14 |  8,000 |     0.005 s |   0.007 s |  0.00% |      57% |  133,704 |      35,874 |
| Worker  p13 | 30,000 |     0.004 s |   0.006 s |  0.00% |      62% |  501,484 |     731,549 |
| Worker  p14 | 30,000 |     0.004 s |   0.006 s |  0.00% |      62% |  501,733 |     706,517 |

### Worker mode, 30,000 users

| Share of the controller | Like p13 | Like p14 | Post p13 | Post p14 |
|---|--:|--:|--:|--:|
| SQLite: PDO prepare, execute, fetch, commit | 41.4% | 79.0% | 56.1% | 74.6% |
| ORM hydration | 10.5% | | | |
| ORM, besides SQLite (find, flush, persist, reading `created_at` back) | 40.3% | 1.2% | 32.2% | 0.7% |
| DBAL, besides SQLite (parameters, results) | | 2.1% | | 3.7% |
| JSON response (JsonStreamer and its writers, `JsonResponse`) | 5.7% | 14.9% | 8.8% | 15.2% |
| Controller, other | 2.1% | 2.7% | 2.9% | 5.8% |
| **Samples** | **62,375** | **26,199 (-58%)** | **7,903** | **5,205 (-34%)** |

- the ORM's work goes: 31,691 samples in the like (hydration, `find()`, `findOneBy()`, `flush()`), 2,541 in the post,
  4.7% of phase 13's PHP samples, against PROFILE-p12.md's 4.8%;
- SQLite's work shrinks as well, 30,244 to 24,594 samples: a like was BEGIN, a SELECT of the post, a SELECT of the
  like, an INSERT and a COMMIT, it's now one autocommitted `INSERT ... SELECT ... ON CONFLICT DO NOTHING`; a post was
  an INSERT then a SELECT of `created_at`, it's now `INSERT ... RETURNING`;
- together: 38,874 fewer samples in the writes, 5.3% of phase 13's PHP samples. All of PHP's samples went down 3.4%:
  the reads, which didn't change, vary by a couple of percent between runs.

`HttpKernel::handle` goes down 5%, and the server's hold CPU doesn't move (62%): the writes are about 8% of the
requests, and PHP isn't all of the server's CPU (Caddy, Go, the kernel).

### Classic mode, 8,000 users

The like's samples go down 60% (the ORM was about half of them); the post's don't (1,125 -> 1,186, too few samples to
read). Classic mode's CPU profile undercounts anyway (PROFILE-p12.md, PROFILE-p13.md): `/proc` below has the sizes.

## CPU, per endpoint

FrankenPHP's CPU in `/proc` (all its threads, Go included), 1,000 sequential requests of each kind after 300 to warm
up (`profile/percpu.sh`, classic mode with the phase's `local-bench/Caddyfile.classic`):

| ms of CPU per request | Classic p13 | Classic p14 | Worker p13 | Worker p14 |
|---|--:|--:|--:|--:|
| `GET /feed` | 0.47 | 0.47 | 0.17 | 0.17 |
| `GET /posts/{id}` | 0.44 | 0.43 | 0.14 | 0.14 |
| `POST /posts/{id}/like` | 0.71 | 0.52 (-27%) | 0.27 | 0.20 (-26%) |
| `POST /posts` | 0.78 | 0.71 (-9%) | 0.31 | 0.27 (-13%) |
| `mix` (feed, post, a like one time in 7) | 1.04 | 0.97 (-7%) | 0.35 | 0.35 |

A like saves 0.07 ms of CPU in worker mode, 0.19 ms in classic mode; a lone user gains as much, whatever the load.
`mix` doesn't move: a like one time in 7 is 0.01 ms per loop, the resolution of `/proc` here.

## Next

- Worker mode: stopped by memory, Caddy's and Go's per open connection (PROFILE-p12.md), not by PHP.
- Classic mode: Symfony's boot, half of PHP's wall time per request (PROFILE-p13.md).
