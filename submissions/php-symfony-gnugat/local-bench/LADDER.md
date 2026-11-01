# The ladder

How the app does, one change at a time, in both FrankenPHP modes: classic (Symfony boots on every request, like
PHP-FPM) and worker (Symfony boots once per worker). One commit per phase, each with its numbers.

**Setup**: `local-bench/bench.sh` runs the server in an Ubuntu 24.04 container on one core of an Apple silicon Mac
(OrbStack), with 2 GB and no swap, **throttled to a quarter of that core** (`--cpus=0.25`); k6 1.4.0 runs
`bench/load.js` in its own container on the other cores. At a full core, the Mac's CPU sits near 50% at any load and
hides differences the leaderboard's box shows: PR 40 scored 35% more users than this app's scored commit there,
and only throttled and saturated does it serve more requests here too (+34.6%, `local-bench/compare-pr40.sh`).

Two measures, both throttled:

- **Users** (`local-bench/limits.sh`), the score as the repo's README gives it: a fresh server, a 1,000-user 2 min
  warm-up, then a 60 s ramp-up, a 5 min hold and a 30 s ramp-down; a run passes with p95 < 0.5 s, p99 < 1 s and
  < 1% errors. The search narrows down to within 250 users, then one more run confirms the limit, stepping down 250
  users until one passes. Only its start differs from the README's (2,500 users, doubling): a guess from the
  requests per second below (about 9 users each), stepping 250 users from it, doubling the step.
- **Requests served per second** (`local-bench/ladder.sh`), saturated: above its capacity, a server serves what it
  can, so the requests it served (total minus failed, over the 3 min run) measure its capacity, more finely than 250
  users. Each setup's load calibrates itself between saturation and timeouts. Two rounds, every setup once per
  round.

Measured 2026-10-30 to 2026-11-01, on FrankenPHP 1.13.0 (PHP 8.5.11) and Symfony 8.1.8, before the update to
FrankenPHP 1.13.4 (PHP 8.5.12) and Symfony 8.1.10 for their security fixes.

**Only compare these numbers with each other.** The leaderboard's box is not a quarter of this core: PR 40 scores
21,000 users there and 5,750 here (3.65 times as many), this app's scored commit (`48e3783`) 15,500 and 3,750 (4.13
times). At those ratios phase 15 would score 20,000 to 22,700 there, a rough range: the two ratios already differ by
13%, and one step here is 900 to 1,000 users there.

## Summary

| Phase | Change | Classic users | Worker users | Classic req/s | Worker req/s |
|---|---|--:|--:|--:|--:|
| 0 | Idiomatic Symfony 8.1, FrankenPHP 1.13's release binary, defaults | 250 | 1,000 | 39 | 117 |
| 1 | php.ini production settings (OPcache, realpath) | 250 | 1,000 | 41 (+5.0%) | 116 (-0.9%) |
| 2 | Preloading, authoritative class map | 250 | 1,000 | 48 (+17.0%) | 125 (+7.2%) |
| 3 | JIT (tracing) | 250 | 1,250 | 50 (+2.8%) | 163 (+30.4%) |
| 4 | Firewall on `POST /posts` only | 250 | 1,250 | 50 (+1.1%) | 162 (-0.5%) |
| 5 | Every request to Symfony (`rewrite` + `php`) | 250 | 1,250 | 51 (+1.1%) | 164 (+1.3%) |
| 6 | DQL read model | 500 | 2,250 | 67 (+32.5%) | 273 (+66.4%) |
| 7 | JsonStreamer | 500 | 3,000 | 81 (+20.0%) | 366 (+34.1%) |
| 8 | DBAL read model | 750 | 4,000 | 88 (+9.8%) | 479 (+31.1%) |
| 9 | Persistent SQLite connections | 1,000 | 4,000 | 132 (+49.0%) | 466 (-2.8%) |
| 10 | DBAL writes | 1,000 | 4,500 | 132 (+0.0%) | 529 (+13.5%) |
| 11 | No ORM | 1,500 | 4,500 | 177 (+34.6%) | 539 (+2.0%) |
| 12 | `CreatePostPayload::fromRequest()` | 1,500 | 4,500, again 4,750 | 183 (+3.3%) | 590 (+9.4%) |
| 13 | Prepared-statement reuse | 1,500 | 5,250 | 180 (-1.5%) | 612 (+3.7%) |
| 14 | SQLite writes the reads' JSON | 1,500 | 5,250 | 184 (+2.1%) | 633 (+3.4%) |
| 15 | The worker loop in `public/index.php` | 1,750 | 5,500 | 201 (+9.2%) | 664 (+4.9%) |

Requests per second are the mean of two rounds, against the phase before. Within 250 users, small changes don't move
the users (phases 0 to 2, 3 to 5): their latencies at the failing step do, below.

**What stops it**: CPU, in every mode and phase. Every failing run is at the throttle's 25-27% CPU, with at most
0.33 GiB of the 2 GB, and none fails on errors: on latency.

**Persistent connections** (phase 9) are classic mode's: there every request opened the database file again, set the
pragmas and read the schema, and closing the last connection made SQLite checkpoint and delete the WAL. Worker mode
already kept its connection across requests.

## Requests per second

| Phase | Worker, rounds | Classic, rounds |
|---|--:|--:|
| 0 | 117 (116 / 119) | 39 (39 / 39) |
| 1 | 116 (116 / 117) | 41 (41 / 41) |
| 2 | 125 (124 / 126) | 48 (48 / 48) |
| 3 | 163 (162 / 164) | 50 (50 / 50) |
| 4 | 162 (158 / 166) | 50 (49 / 51) |
| 5 | 164 (158 / 170) | 51 (49 / 52) |
| 6 | 273 (276 / 269) | 67 (67 / 67) |
| 7 | 366 (372 / 360) | 81 (81 / 80) |
| 8 | 479 (482 / 476) | 88 (87 / 90) |
| 9 | 466 (458 / 474) | 132 (132 / 131) |
| 10 | 529 (525 / 532) | 132 (132 / 132) |
| 11 | 539 (538 / 541) | 177 (179 / 176) |
| 12 | 590 (589 / 591) | 183 (183 / 183) |
| 13 | 612 (596 / 627) | 180 (182 / 179) |
| 14 | 633 (625 / 640) | 184 (184 / 184) |
| 15 | 664 (646 / 681) | 201 (201 / 202) |

`local-bench/ladder.sh build` builds one image per phase commit, `ladder.sh run 2` runs both rounds. Loads: worker
3,000 users up to phase 5 and 10,000 from phase 6, classic 2,000 up to phase 8 and 4,000 from phase 9. Where the load
changes, the phase before was run again at the higher load, and the phase after at the lower one: phase 8 classic
serves 90 per second at 4,000 users (87 and 90 at 2,000), phase 9 classic 128 at 2,000 (132 and 131 at 4,000),
phase 6 worker 252 at 3,000 (276 and 269 at 10,000). Far above saturation it drops: phase 5 worker at 10,000 users
has 13.6% timeouts and serves 125 per second (158 and 170 at 3,000).

## Experiments, not kept

On phase 15, with `local-bench/experiment.sh compare` and the files in `local-bench/experiments/`, throttled and
saturated like the ladder (10,000 users in worker mode, 4,000 in classic), against phase 15 in the same session, two
rounds. Phase 15 itself moves by about 2% between rounds.

| Experiment | What | Worker req/s | Classic req/s |
|---|---|--:|--:|
| `no-gc` | `zend.enable_gc=0` | 709 (+7.2%) | |
| `pr40-caddyfile` | PR 40's Caddyfile settings | 686 (+3.7%) | |
| `gogc-200` | `GOGC=200` | 680 (+2.8%), 0.13 GiB more | |
| `output-buffering-0` | `output_buffering=0`, as PR 40 | 671 (+1.5%) | |
| `gomemlimit` | `GOMEMLIMIT=1100MiB` | 663 (+0.2%) | |
| `cgocheck-0` | `GODEBUG=cgocheck=0`, as PR 40 | 658 (-0.5%) | |
| `workers-1` | 1 worker | 659 (-0.4%) | |
| `workers-4` | 4 workers | 658 (-0.4%) | |
| `max-threads-8` | `max_threads 8` | 640 (-3.2%) | 194 (-3.1%) |
| `num-threads-8` | 8 PHP threads all the time | | 199 (-0.7%) |
| `never-restart` | workers never restart, as PR 40 | 680 (+1.2%) | |
| `sqlite-cache-64` | SQLite `cache_size` 64 MiB, as PR 40 | 674 (+0.4%) | |
| `sqlite-mmap` | SQLite `mmap_size` 1 GiB, as PR 40 | 651 (-3.1%) | |

Phase 15 served 661 and 200 per second there; the three patch experiments ran in a later session, against 672.
Only `zend.enable_gc=0` is above the noise, and in one round of two (687, 730): without the cycle collector, a
leaking cycle would grow a worker until its restart, for a gain within a step.

## Peers, same setup

Submissions with leaderboard scores, from the repo's `main` (`bench.sh build-peer`), and this app's scored commit
(`bench.sh build-peer php-symfony-gnugat 48e3783`), searched with `GUESS=... limits.sh run <folder> peer`.

| Peer | Leaderboard | Here | Stops here on |
|---|--:|--:|---|
| PR 40, PHP Symfony + FrankenPHP | 21,000 | 5,750 | CPU |
| PR 24, PHP Swoole | 71,000 (open files) | 29,250 | CPU |
| This app's scored commit, `48e3783` | 15,500 | 3,750 | CPU |

The box stopped Swoole at the rules' 65,535 open files with its CPU at 66%; here, a quarter of a core, the CPU stops
it first. PR 40 passed 6,000 users once in the search, not in the confirming run. PR 40 holds 1.53 times the scored
commit's users here, 1.35 times on the box: the throttle doesn't scale every app alike. The scored commit still has
FrankenPHP's deb package, `max_threads 8`, the ORM, `#[MapRequestPayload]` and `symfony/runtime`'s runner, so it
isn't one of the phases: phase 15 holds 47% more users.

|                    |  Users | p95 < 0.5 s |  p99 < 1 s | Errors | Hold CPU | Peak RAM  |
|--------------------|-------:|------------:|-----------:|-------:|---------:|----------:|
| PR 40, Symfony     |  5,750 |     0.268 s |    0.410 s |  0.00% |      27% | 0.257 GiB |
| PR 40, Symfony     |  6,000 |   ❌ 1.660 s |  ❌ 2.210 s |  0.00% |      27% | 0.290 GiB |
| PR 24, Swoole      | 29,250 |     0.191 s |    0.279 s |  0.00% |      27% | 0.719 GiB |
| PR 24, Swoole      | 29,500 |     0.341 s |  ❌ 1.070 s |  0.00% |      28% | 0.716 GiB |
| This app, 48e3783  |  3,750 |     0.404 s |    0.886 s |  0.00% |      27% | 0.257 GiB |
| This app, 48e3783  |  4,000 |   ❌ 0.573 s |  ❌ 1.000 s |  0.00% |      27% | 0.270 GiB |

## All runs

The last passing level (the confirming run) and the next one, 250 users above, failing. Phase 12's worker limit
sits at the boundary: the first search failed 4,750 on its p99 alone, a second one (`again`) passed it twice.

|                    |  Users | p95 < 0.5 s |  p99 < 1 s | Errors | Hold CPU | Peak RAM  |
|--------------------|-------:|------------:|-----------:|-------:|---------:|----------:|
| Classic p0         |    250 |     0.126 s |    0.218 s |  0.00% |      21% | 0.115 GiB |
| Classic p0         |    500 |   ❌ 3.770 s |  ❌ 4.040 s |  0.00% |      26% | 0.112 GiB |
| Worker  p0         |  1,000 |     0.248 s |    0.720 s |  0.00% |      26% | 0.100 GiB |
| Worker  p0         |  1,250 |   ❌ 1.180 s |  ❌ 1.600 s |  0.00% |      26% | 0.115 GiB |
| Classic p1         |    250 |     0.105 s |    0.179 s |  0.00% |      20% | 0.117 GiB |
| Classic p1         |    500 |   ❌ 2.660 s |  ❌ 2.930 s |  0.00% |      26% | 0.115 GiB |
| Worker  p1         |  1,000 |     0.246 s |    0.406 s |  0.00% |      26% | 0.101 GiB |
| Worker  p1         |  1,250 |   ❌ 0.979 s |  ❌ 1.100 s |  0.00% |      26% | 0.115 GiB |
| Classic p2         |    250 |     0.054 s |    0.092 s |  0.00% |      18% | 0.124 GiB |
| Classic p2         |    500 |   ❌ 2.090 s |  ❌ 2.570 s |  0.00% |      26% | 0.127 GiB |
| Worker  p2         |  1,000 |     0.194 s |    0.549 s |  0.00% |      26% | 0.118 GiB |
| Worker  p2         |  1,250 |   ❌ 1.030 s |  ❌ 1.380 s |  0.00% |      26% | 0.132 GiB |
| Classic p3         |    250 |     0.065 s |    0.165 s |  0.00% |      17% | 0.137 GiB |
| Classic p3         |    500 |   ❌ 1.100 s |  ❌ 1.310 s |  0.00% |      26% | 0.137 GiB |
| Worker  p3         |  1,250 |     0.171 s |    0.355 s |  0.00% |      26% | 0.139 GiB |
| Worker  p3         |  1,500 |   ❌ 0.587 s |    0.708 s |  0.00% |      26% | 0.150 GiB |
| Classic p4         |    250 |     0.057 s |    0.092 s |  0.00% |      18% | 0.141 GiB |
| Classic p4         |    500 |   ❌ 0.766 s |    0.948 s |  0.00% |      26% | 0.139 GiB |
| Worker  p4         |  1,250 |     0.151 s |    0.212 s |  0.00% |      26% | 0.138 GiB |
| Worker  p4         |  1,500 |   ❌ 0.568 s |    0.903 s |  0.00% |      26% | 0.155 GiB |
| Classic p5         |    250 |     0.049 s |    0.090 s |  0.00% |      17% | 0.135 GiB |
| Classic p5         |    500 |   ❌ 0.611 s |    0.743 s |  0.00% |      26% | 0.131 GiB |
| Worker  p5         |  1,250 |     0.143 s |    0.229 s |  0.00% |      26% | 0.139 GiB |
| Worker  p5         |  1,500 |   ❌ 1.240 s |  ❌ 1.840 s |  0.00% |      26% | 0.153 GiB |
| Classic p6         |    500 |     0.245 s |    0.367 s |  0.00% |      25% | 0.134 GiB |
| Classic p6         |    750 |   ❌ 2.340 s |  ❌ 2.500 s |  0.00% |      26% | 0.131 GiB |
| Worker  p6         |  2,250 |     0.157 s |    0.233 s |  0.00% |      27% | 0.182 GiB |
| Worker  p6         |  2,500 |   ❌ 0.674 s |  ❌ 1.240 s |  0.00% |      27% | 0.202 GiB |
| Classic p7         |    500 |     0.073 s |    0.145 s |  0.00% |      23% | 0.128 GiB |
| Classic p7         |    750 |   ❌ 0.634 s |    0.772 s |  0.00% |      26% | 0.129 GiB |
| Worker  p7         |  3,000 |     0.266 s |    0.380 s |  0.00% |      27% | 0.218 GiB |
| Worker  p7         |  3,250 |   ❌ 0.551 s |    0.863 s |  0.00% |      27% | 0.233 GiB |
| Classic p8         |    750 |     0.277 s |    0.376 s |  0.00% |      26% | 0.125 GiB |
| Classic p8         |  1,000 |   ❌ 2.830 s |  ❌ 3.200 s |  0.00% |      26% | 0.132 GiB |
| Worker  p8         |  4,000 |     0.433 s |    0.582 s |  0.00% |      27% | 0.264 GiB |
| Worker  p8         |  4,250 |   ❌ 0.909 s |  ❌ 1.240 s |  0.00% |      27% | 0.284 GiB |
| Classic p9         |  1,000 |     0.187 s |    0.648 s |  0.00% |      26% | 0.126 GiB |
| Classic p9         |  1,250 |   ❌ 0.911 s |  ❌ 1.090 s |  0.00% |      26% | 0.136 GiB |
| Worker  p9         |  4,000 |     0.390 s |    0.530 s |  0.00% |      27% | 0.263 GiB |
| Worker  p9         |  4,250 |   ❌ 0.579 s |    0.785 s |  0.00% |      27% | 0.277 GiB |
| Classic p10        |  1,000 |     0.127 s |    0.234 s |  0.00% |      26% | 0.124 GiB |
| Classic p10        |  1,250 |   ❌ 0.650 s |  ❌ 1.910 s |  0.00% |      26% | 0.140 GiB |
| Worker  p10        |  4,500 |     0.431 s |    0.593 s |  0.00% |      27% | 0.290 GiB |
| Worker  p10        |  4,750 |   ❌ 0.768 s |    0.993 s |  0.00% |      27% | 0.313 GiB |
| Classic p11        |  1,500 |     0.274 s |    0.902 s |  0.00% |      26% | 0.142 GiB |
| Classic p11        |  1,750 |   ❌ 0.999 s |  ❌ 1.350 s |  0.00% |      26% | 0.158 GiB |
| Worker  p11        |  4,500 |     0.449 s |    0.643 s |  0.00% |      27% | 0.289 GiB |
| Worker  p11        |  4,750 |   ❌ 1.610 s |  ❌ 2.370 s |  0.00% |      27% | 0.313 GiB |
| Classic p12        |  1,500 |     0.174 s |    0.282 s |  0.00% |      26% | 0.132 GiB |
| Classic p12        |  1,750 |   ❌ 0.509 s |    0.653 s |  0.00% |      26% | 0.144 GiB |
| Classic p12, again |  1,500 |     0.171 s |    0.242 s |  0.00% |      26% | 0.132 GiB |
| Classic p12, again |  1,750 |   ❌ 0.747 s |    0.887 s |  0.00% |      27% | 0.146 GiB |
| Worker  p12        |  4,500 |     0.264 s |    0.419 s |  0.00% |      27% | 0.272 GiB |
| Worker  p12        |  4,750 |     0.429 s |  ❌ 1.580 s |  0.00% |      27% | 0.293 GiB |
| Worker  p12, again |  4,750 |     0.455 s |    0.627 s |  0.00% |      27% | 0.286 GiB |
| Worker  p12, again |  5,000 |   ❌ 0.559 s |    0.787 s |  0.00% |      27% | 0.295 GiB |
| Classic p13        |  1,500 |     0.192 s |    0.279 s |  0.00% |      26% | 0.132 GiB |
| Classic p13        |  1,750 |   ❌ 0.721 s |    0.861 s |  0.00% |      27% | 0.145 GiB |
| Worker  p13        |  5,250 |     0.433 s |    0.644 s |  0.00% |      27% | 0.312 GiB |
| Worker  p13        |  5,500 |   ❌ 0.548 s |    0.773 s |  0.00% |      27% | 0.315 GiB |
| Classic p14        |  1,500 |     0.237 s |    0.356 s |  0.00% |      26% | 0.133 GiB |
| Classic p14        |  1,750 |   ❌ 0.948 s |  ❌ 1.170 s |  0.00% |      26% | 0.160 GiB |
| Worker  p14        |  5,250 |     0.316 s |    0.455 s |  0.00% |      27% | 0.303 GiB |
| Worker  p14        |  5,500 |   ❌ 0.539 s |    0.697 s |  0.00% |      27% | 0.315 GiB |
| Classic p15        |  1,750 |     0.263 s |    0.370 s |  0.00% |      27% | 0.142 GiB |
| Classic p15        |  2,000 |   ❌ 1.220 s |  ❌ 1.410 s |  0.00% |      27% | 0.158 GiB |
| Worker  p15        |  5,500 |     0.398 s |    0.596 s |  0.00% |      27% | 0.316 GiB |
| Worker  p15        |  5,750 |   ❌ 0.545 s |  ❌ 1.110 s |  0.00% |      27% | 0.327 GiB |
