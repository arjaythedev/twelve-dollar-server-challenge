# Profiling phase 13

Phase 13 (persistent SQLite connections) measured after the fact, against phase 12 (PROFILE-p12.md). The question:
phase 12's profile pointed at Doctrine's writes, not at the connections, yet this phase is the ladder's biggest step
in classic mode (17,500 -> 29,500 users, +69%), for 19% less CPU per request. Where did the rest come from? Measured
2026-10-21, same setup as LADDER.md.

**In short**: from waiting, which a CPU profile can't see. Sampled in wall-clock time, phase 12's classic mode spends
a third of PHP's time in `connect` (the pragmas, which read the schema) and a third in commits: a like takes 3.9 ms,
for 1.8 ms of CPU. Every request closed the last connection to the database, and SQLite then checkpoints and deletes
the WAL, which the next request has to set up again. With one connection kept per PHP thread, the WAL stays: a like
takes 0.27 ms, and PHP's time per request goes from 0.65 ms to 0.26 ms. Worker mode, which kept its connection
already, doesn't change.

## Wall-clock time

Excimer samples the CPU clock by default, and in classic mode a CPU profile undercounts (PROFILE-p12.md): each request
starts its own timer, and the kernel only checks it on its tick, about every 1 ms. `EXCIMER_EVENT=real` samples
wall-clock time instead, with the kernel's high-resolution timers: waits included (SQLite's busy handler, file locks,
reads), and no undercount. Here it's 0.65 ms of PHP per request, against 0.6 ms of CPU for the whole server in `/proc`
(below), where the CPU profile has 0.08 ms.

Both phases at 8,000 users in classic mode, under the real load mix of `bench/load.js`. Without warm-up, 30 s ramp-up,
2 min hold.

```sh
p12="$(git log --format=%h -1 --grep='^php-symfony-gnugat: phase 12,')"
git worktree add "$TMPDIR/p12" "$p12"
IMAGE=twelve-dollar-php-symfony:p12 bash "$TMPDIR/p12/submissions/php-symfony-gnugat/local-bench/bench.sh" build
docker build --build-arg BASE=twelve-dollar-php-symfony:p12 -f local-bench/profile/Dockerfile \
  -t twelve-dollar-php-symfony-profile:p12 local-bench/profile
# the same for this phase (p13), from this checkout; then for each phase:
IMAGE=twelve-dollar-php-symfony-profile:p12 \
  SERVE_ARGS="-e EXCIMER_EVENT=real -v $PWD/local-bench/profile/Caddyfile:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 8000 p12-classic-real
mkdir p12-classic-real && docker cp twelve-dollar-server:/tmp/profile/stacks.txt p12-classic-real/
cp local-bench/results/*-p12-classic-real-8000.k6 p12-classic-real/k6.txt
python3 -I local-bench/profile/compare.py p12-classic-real p13-classic-real HttpKernel::handle
```

|                   | Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | PHP samples |
|-------------------|------:|------------:|----------:|-------:|---------:|---------:|------------:|
| Classic p12, wall | 8,000 |     0.008 s |   0.014 s |  0.00% |      52% |  133,816 |     863,330 |
| Classic p13, wall | 8,000 |     0.005 s |   0.008 s |  0.00% |      57% |  133,865 |     342,103 |

| Share of `HttpKernel::handle`, wall-clock time | p12 | p13 |
|---|--:|--:|
| `connect`: open, pragmas, schema read | 36.2% | 1.9% |
| SQLite: PDO prepare, execute, fetch, commit | 50.1% | 18.5% |
| ORM, besides SQLite (find, flush, query building) | 5.2% | 29.7% |
| JSON, DBAL and the rest | 8.5% | 49.9% |
| **Samples per 1,000 requests** | **5,809** | **1,250 (-78%)** |

`HttpKernel::handle` is 90% of PHP's wall time in phase 12, 49% in phase 13: the other half is now Symfony's boot,
on every request in classic mode.

Per request (each endpoint's count estimated from k6's requests and the load mix: feed and post 1 each, like 0.15,
post 0.02):

| ms of PHP wall-clock time | p12 | p13 |
|---|--:|--:|
| `GET /feed` | 0.25 | 0.06 |
| `GET /posts/{id}` | 0.25 | 0.05 |
| `POST /posts/{id}/like` | 3.9 | 0.27 |
| `POST /posts` | 4.3 | 0.27 |
| `connect`, per request | 0.21 | 0.00 |
| `PDO\Connection::commit`, per write | 3.0 | 0.13 |

- `connect` is the pragmas: 31.5% of PHP's wall time is `PDO\Connection::exec` under `SqlitePragmasMiddleware`, the
  open itself (`SQLite\Driver::doConnect`) 0.9%. `PRAGMA synchronous` needs the schema, so the first pragma on a new
  connection reads it: a read transaction, which opens the WAL and its shared memory, and waits on their locks;
- the commits: 3 ms of wall time per write, for a fraction of that in CPU. In WAL mode, when the last connection to a
  database closes, SQLite checkpoints the WAL into the database and deletes it. In phase 12, a request's connection is
  often the only one open, and each write then starts from no WAL at all, while the checkpoints hold locks the other
  PHP thread waits on (SQLite's busy handler sleeps in steps of 1 ms and up). Checked, one request at a time: in phase
  12, `feed.db-wal` is gone a second after a like; in phase 13 it stays (49,472 bytes). How the wait splits between
  the checkpoints, setting the WAL up again and the busy handler's sleeps isn't measured;
- classic mode has 2 PHP threads here (FrankenPHP's default, before phase 15): a thread waiting 4 ms on a write is
  a thread not serving reads. That's why phase 12's classic mode failed at 84% CPU, and phase 13's fails with the core
  full (107%).

## CPU, per endpoint

FrankenPHP's CPU in `/proc` (all its threads, Go included), 1,000 sequential requests of each kind after 300 to warm
up (`profile/percpu.sh`, classic mode with the phase's `local-bench/Caddyfile.classic`):

| ms of CPU per request | Classic p12 | Classic p13 | Worker p12 | Worker p13 |
|---|--:|--:|--:|--:|
| `GET /feed` | 0.59 | 0.47 (-20%) | 0.18 | 0.17 |
| `GET /posts/{id}` | 0.56 | 0.44 (-21%) | 0.15 | 0.14 |
| `POST /posts/{id}/like` | 1.78 | 0.71 (-60%) | 0.30 | 0.27 |
| `POST /posts` | 2.13 | 0.78 (-63%) | 0.31 | 0.31 |
| `mix` (feed, post, a like one time in 7) | 1.28 | 1.04 (-19%) | 0.37 | 0.35 |

The writes lose 1.1-1.4 ms of CPU each, the reads 0.12 ms: the open and the schema read, and for the writes setting up
the WAL again. Worker mode: within the 10 ms resolution of `/proc`, as expected.

Under load the other way round, not explained: classic mode's hold CPU at 8,000 users goes up, 52% to 57%. Perhaps
the WAL: kept between requests, it grows to the automatic checkpoint's 1,000 pages, and a read has more of it to look
through, where phase 12's was often brand new. Not measured.

## CPU, under load

The CPU profiles, for the record (`profile/Caddyfile` and `profile/Caddyfile.worker`, without `EXCIMER_EVENT`):

|                  |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU | Requests | PHP samples |
|------------------|-------:|------------:|----------:|-------:|---------:|---------:|------------:|
| Classic p12, CPU |  8,000 |     0.007 s |   0.012 s |  0.00% |      52% |  133,696 |     104,780 |
| Classic p13, CPU |  8,000 |     0.005 s |   0.008 s |  0.00% |      57% |  133,511 |      44,752 |
| Worker  p12, CPU | 30,000 |     0.004 s |   0.007 s |  0.00% |      61% |  501,648 |     720,563 |
| Worker  p13, CPU | 30,000 |     0.004 s |   0.006 s |  0.00% |      62% |  501,484 |     731,549 |

Classic mode's CPU profile sees `connect` at 15% and commits at 51% of phase 12's samples, the direction but not the
size: its counts are a fraction of the CPU (0.08 ms per request) and lean away from the ends of requests. Worker mode:
the same within 2%.

## Next

- Classic mode: Symfony's boot is now half of PHP's wall time per request. Worker mode is the answer to that, and the
  submission's mode since phase 11.
- Worker mode: unchanged here, still stopped by memory (PROFILE-p12.md). The writes' Doctrine work (4.8% of PHP's CPU,
  PROFILE-p12.md) is still there in both modes: in classic mode, the ORM is now 30% of `HttpKernel::handle`'s wall time.
