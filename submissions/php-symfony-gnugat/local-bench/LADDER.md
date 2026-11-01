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

Users searched 2026-10-30 to 2026-11-01, requests per second measured 2026-11-06, on FrankenPHP 1.13.0 (PHP 8.5.11) and
Symfony 8.1.8.

**Only compare these numbers with each other.** The leaderboard's box is not a quarter of this core: #256, this app's
last scored version (`4a11a5d`), scores 18,500 users there and 5,500 here (3.36 times as many), PR 40 21,000 and 5,750
(3.65 times), this app's first scored commit (`48e3783`) 15,500 and 3,750 (4.13 times). At #256's ratio, the closest
app to these phases, phase 14 would score about 17,700 there, and 19,200 to 21,700 at the other two: a rough estimate,
the ratios differ by 23%, and one step here is 840 to 1,030 users there.

## Summary

| Phase | Change | Classic users | Worker users | Classic req/s | Worker req/s |
|---|---|--:|--:|--:|--:|
| 0 | Idiomatic Symfony 8.1, FrankenPHP 1.13's release binary, defaults | 250 | 1,000 | 38 | 114 |
| 1 | php.ini production settings (OPcache, realpath) | 250 | 1,000 | 39 (+2.6%) | 116 (+2.2%) |
| 2 | Preloading, authoritative class map | 250 | 1,000 | 47 (+20.5%) | 120 (+2.6%) |
| 3 | JIT (tracing) | 250 | 1,250 | 50 (+7.4%) | 155 (+29.7%) |
| 4 | DQL read model | 500 | 2,250 | 63 (+24.8%) | 244 (+57.7%) |
| 5 | JsonStreamer | 500 | 3,000 | 74 (+17.5%) | 342 (+40.1%) |
| 6 | DBAL read model | 750 | 4,000 | 85 (+14.9%) | 422 (+23.4%) |
| 7 | Persistent SQLite connections | 1,000 | 4,000 | 120 (+41.2%) | 429 (+1.5%) |
| 8 | DBAL writes | 1,000 | 4,500 | 122 (+1.2%) | 470 (+9.4%) |
| 9 | No ORM | 1,500 | 4,500 | 156 (+28.8%) | 482 (+2.7%) |
| 10 | `CreatePostPayload::fromRequest()` | 1,500 | 4,500, again 4,750 | 162 (+3.2%) | 513 (+6.4%) |
| 11 | Prepared-statement reuse | 1,500 | 5,250 | 153 (-5.3%) | 569 (+10.9%) |
| 12 | SQLite writes the reads' JSON | 1,500 | 5,250 | 160 (+4.9%) | 592 (+4.0%) |
| 13 | Every request to Symfony (`rewrite` + `php`) | 1,500 | 5,250 | 166 (+3.1%) | 630 (+6.4%) |
| 14 | Firewall on `POST /posts` only | 1,500 | 5,250 | 178 (+7.3%) | 648 (+2.9%) |

Requests per second are the mean of two rounds, against the phase before. Within 250 users, small changes don't move
the users (phases 0 to 2): their latencies at the failing step do, below.

The users were searched in an earlier order of the phases, where phases 13 and 14 came right after the JIT: phases 4
to 12's users are with both in place, phase 13's with phase 14, which the requests per second are not: those were
measured again on this order, and phases 4 to 12 serve 3% to 15% fewer than then: at about 8.6 users per request per
second, one to three steps fewer in worker mode. The earlier order's last phase was phase 14's app, so phases 12 to
14 share its search, which is phase 14's own.

**What stops it**: CPU, in every mode and phase. Every failing run is at the throttle's 25-27% CPU, with at most
0.32 GiB of the 2 GB, and none fails on errors: on latency.

**Persistent connections** (phase 7) are classic mode's: there every request opened the database file again, set the
pragmas and read the schema, and closing the last connection made SQLite checkpoint and delete the WAL. Worker mode
already kept its connection across requests.

## Requests per second

| Phase | Worker, rounds | Classic, rounds |
|---|--:|--:|
| 0 | 114 (115 / 113) | 38 (39 / 37) |
| 1 | 116 (115 / 118) | 39 (39 / 39) |
| 2 | 120 (121 / 118) | 47 (47 / 47) |
| 3 | 155 (154 / 156) | 50 (49 / 52) |
| 4 | 244 (240 / 249) | 63 (63 / 63) |
| 5 | 342 (334 / 351) | 74 (72 / 76) |
| 6 | 422 (429 / 416) | 85 (84 / 86) |
| 7 | 429 (425 / 433) | 120 (123 / 117) |
| 8 | 470 (478 / 461) | 122 (121 / 122) |
| 9 | 482 (483 / 481) | 156 (155 / 158) |
| 10 | 513 (509 / 517) | 162 (158 / 165) |
| 11 | 569 (557 / 581) | 153 (150 / 156) |
| 12 | 592 (582 / 602) | 160 (158 / 163) |
| 13 | 630 (623 / 637) | 166 (168 / 163) |
| 14 | 648 (663 / 633) | 178 (178 / 177) |

`local-bench/ladder.sh build` builds one image per phase commit, `ladder.sh run 2` runs both rounds (`PHASES=13`, then
`PHASES=14`, for phases 13 and 14 alone, which ran after the others, the same day). Loads: worker 3,000 users up to
phase 3 and 10,000 from phase 4, classic 2,000 up to phase 6 and 4,000 from phase 7, each saturated without timeouts.

## Experiments, not kept

On phase 14's app, as the last phase of the earlier order, with `local-bench/experiment.sh compare` and the files in
`local-bench/experiments/`, throttled and saturated like the ladder (10,000 users in worker mode, 4,000 in classic),
against phase 14 in the same session, two rounds. It served 637 and 641 per second in worker mode, 180 and 180 in
classic, but an experiment's two rounds can differ by more: up to 8% (`gogc-200`, 661 and 612).

| Experiment | What | Worker req/s | Classic req/s |
|---|---|--:|--:|
| `no-gc` | `zend.enable_gc=0` | 695 (+8.8%) | |
| `workers-1` | 1 worker | 665 (+4.1%) | |
| `gomemlimit` | `GOMEMLIMIT=1100MiB` | 662 (+3.6%) | |
| `own-worker-loop` | FrankenPHP's worker loop, without `symfony/runtime` | 655 (+2.5%) | 191 (+6.0%) |
| `sqlite-cache-64` | SQLite `cache_size` 64 MiB, as PR 40 | 652 (+2.1%) | |
| `pr40-caddyfile` | PR 40's Caddyfile settings | 651 (+2.0%) | |
| `never-restart` | workers never restart (`FRANKENPHP_LOOP_MAX=0`), as PR 40 | 646 (+1.1%) | |
| `cgocheck-0` | `GODEBUG=cgocheck=0`, as PR 40 | 642 (+0.5%) | |
| `output-buffering-0` | `output_buffering=0`, as PR 40 | 642 (+0.5%) | |
| `gogc-200` | `GOGC=200` | 637 (-0.3%), 0.2 GiB more | |
| `sqlite-mmap` | SQLite `mmap_size` 1 GiB, as PR 40 | 625 (-2.1%) | |
| `workers-4` | 4 workers | 622 (-2.6%) | |
| `max-threads-8` | `max_threads 8` | 616 (-3.5%) | 177 (-2.1%) |
| `num-threads-8` | 8 PHP threads all the time | | 176 (-2.5%) |

Only `zend.enable_gc=0` is clearly above the noise, in both rounds (691, 699): without the cycle collector, a leaking
cycle would grow a worker until its restart, for a gain within a step. `workers-1` (656, 673) is at its edge.

Phases 13 and 14 were also measured as experiments on phase 12, against phase 12 (585 and 154 per second there), in the
same session: `rewrite * /index.php` + `php` +5.2% in worker mode (619, 611) and +5.5% in classic (160, 165), the
firewall on `POST /posts` only +0.2% in worker mode (599, 573) and +10.4% in classic (170, 169), where Symfony boots on
every request; and the Caddyfile experiments, without the rewrite: 1 worker +3.2%, 4 workers -3.8%, `max_threads 8`
-4.2% (+0.1% in classic), 8 PHP threads -1.1% in classic.

`own-worker-loop` is the loop of an earlier phase 15, dropped. That phase kept `symfony/runtime` installed: as long as
the `SymfonyRuntime` class exists, `FrameworkBundle::boot()` leaves registering Symfony's `ErrorHandler` to
`symfony/runtime`, so a loop that bypasses it runs without one, and was faster for it. Against phase 14, three rounds in
worker mode and two in classic: that phase 15 served 2.7% more in worker mode and 8.8% more in classic, and the same
loop with `symfony/runtime` removed, so that FrameworkBundle registers the `ErrorHandler` itself, 2.5% less and 4.7%
more. In worker mode, the scored one, it's within the noise: -2.5% there, +2.5% here.

## Peers, same setup

Submissions with leaderboard scores, from the repo's `main` (`bench.sh build-peer`), and this app's first scored commit
(`bench.sh build-peer php-symfony-gnugat 48e3783`), searched with `GUESS=... limits.sh run <folder> peer`. #256's
commit (`4a11a5d`) is an earlier phase 15 (its own worker loop, without Symfony's `ErrorHandler`, above) updated to
FrankenPHP 1.13.4: its users here are that phase's, searched on FrankenPHP 1.13.0.

| Peer | Leaderboard | Here | Stops here on |
|---|--:|--:|---|
| PR 40, PHP Symfony + FrankenPHP | 21,000 | 5,750 | CPU |
| PR 24, PHP Swoole | 71,000 (open files) | 29,250 | CPU |
| This app's first scored commit, `48e3783` | 15,500 | 3,750 | CPU |
| This app's last scored commit, `4a11a5d` (#256) | 18,500 | 5,500 | CPU |

The box stopped Swoole at the rules' 65,535 open files with its CPU at 66%; here, a quarter of a core, the CPU stops
it first. PR 40 passed 6,000 users once in the search, not in the confirming run. PR 40 holds 1.53 times the scored
commit's users here, 1.35 times on the box: the throttle doesn't scale every app alike. The scored commit still has
FrankenPHP's deb package, `max_threads 8`, the ORM, `#[MapRequestPayload]` and `symfony/runtime`'s runner, so it
isn't one of the phases: phase 14 holds 40% more users.

|                    |  Users | p95 < 0.5 s |  p99 < 1 s | Errors | Hold CPU | Peak RAM  |
|--------------------|-------:|------------:|-----------:|-------:|---------:|----------:|
| PR 40, Symfony     |  5,750 |     0.268 s |    0.410 s |  0.00% |      27% | 0.257 GiB |
| PR 40, Symfony     |  6,000 |   ❌ 1.660 s |  ❌ 2.210 s |  0.00% |      27% | 0.290 GiB |
| PR 24, Swoole      | 29,250 |     0.191 s |    0.279 s |  0.00% |      27% | 0.719 GiB |
| PR 24, Swoole      | 29,500 |     0.341 s |  ❌ 1.070 s |  0.00% |      28% | 0.716 GiB |
| This app, 48e3783  |  3,750 |     0.404 s |    0.886 s |  0.00% |      27% | 0.257 GiB |
| This app, 48e3783  |  4,000 |   ❌ 0.573 s |  ❌ 1.000 s |  0.00% |      27% | 0.270 GiB |

## All runs

The last passing level (the confirming run) and the next one, 250 users above, failing, searched in the earlier order:
phases 4 to 12 with phases 13 and 14 in place, and phase 12's rows are phases 13 and 14's too (above). Phase 10's worker
limit sits at the boundary: the first search failed 4,750 on its p99 alone, a second one (`again`) passed it twice.

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
| Classic p4         |    500 |     0.245 s |    0.367 s |  0.00% |      25% | 0.134 GiB |
| Classic p4         |    750 |   ❌ 2.340 s |  ❌ 2.500 s |  0.00% |      26% | 0.131 GiB |
| Worker  p4         |  2,250 |     0.157 s |    0.233 s |  0.00% |      27% | 0.182 GiB |
| Worker  p4         |  2,500 |   ❌ 0.674 s |  ❌ 1.240 s |  0.00% |      27% | 0.202 GiB |
| Classic p5         |    500 |     0.073 s |    0.145 s |  0.00% |      23% | 0.128 GiB |
| Classic p5         |    750 |   ❌ 0.634 s |    0.772 s |  0.00% |      26% | 0.129 GiB |
| Worker  p5         |  3,000 |     0.266 s |    0.380 s |  0.00% |      27% | 0.218 GiB |
| Worker  p5         |  3,250 |   ❌ 0.551 s |    0.863 s |  0.00% |      27% | 0.233 GiB |
| Classic p6         |    750 |     0.277 s |    0.376 s |  0.00% |      26% | 0.125 GiB |
| Classic p6         |  1,000 |   ❌ 2.830 s |  ❌ 3.200 s |  0.00% |      26% | 0.132 GiB |
| Worker  p6         |  4,000 |     0.433 s |    0.582 s |  0.00% |      27% | 0.264 GiB |
| Worker  p6         |  4,250 |   ❌ 0.909 s |  ❌ 1.240 s |  0.00% |      27% | 0.284 GiB |
| Classic p7         |  1,000 |     0.187 s |    0.648 s |  0.00% |      26% | 0.126 GiB |
| Classic p7         |  1,250 |   ❌ 0.911 s |  ❌ 1.090 s |  0.00% |      26% | 0.136 GiB |
| Worker  p7         |  4,000 |     0.390 s |    0.530 s |  0.00% |      27% | 0.263 GiB |
| Worker  p7         |  4,250 |   ❌ 0.579 s |    0.785 s |  0.00% |      27% | 0.277 GiB |
| Classic p8         |  1,000 |     0.127 s |    0.234 s |  0.00% |      26% | 0.124 GiB |
| Classic p8         |  1,250 |   ❌ 0.650 s |  ❌ 1.910 s |  0.00% |      26% | 0.140 GiB |
| Worker  p8         |  4,500 |     0.431 s |    0.593 s |  0.00% |      27% | 0.290 GiB |
| Worker  p8         |  4,750 |   ❌ 0.768 s |    0.993 s |  0.00% |      27% | 0.313 GiB |
| Classic p9         |  1,500 |     0.274 s |    0.902 s |  0.00% |      26% | 0.142 GiB |
| Classic p9         |  1,750 |   ❌ 0.999 s |  ❌ 1.350 s |  0.00% |      26% | 0.158 GiB |
| Worker  p9         |  4,500 |     0.449 s |    0.643 s |  0.00% |      27% | 0.289 GiB |
| Worker  p9         |  4,750 |   ❌ 1.610 s |  ❌ 2.370 s |  0.00% |      27% | 0.313 GiB |
| Classic p10        |  1,500 |     0.174 s |    0.282 s |  0.00% |      26% | 0.132 GiB |
| Classic p10        |  1,750 |   ❌ 0.509 s |    0.653 s |  0.00% |      26% | 0.144 GiB |
| Classic p10, again |  1,500 |     0.171 s |    0.242 s |  0.00% |      26% | 0.132 GiB |
| Classic p10, again |  1,750 |   ❌ 0.747 s |    0.887 s |  0.00% |      27% | 0.146 GiB |
| Worker  p10        |  4,500 |     0.264 s |    0.419 s |  0.00% |      27% | 0.272 GiB |
| Worker  p10        |  4,750 |     0.429 s |  ❌ 1.580 s |  0.00% |      27% | 0.293 GiB |
| Worker  p10, again |  4,750 |     0.455 s |    0.627 s |  0.00% |      27% | 0.286 GiB |
| Worker  p10, again |  5,000 |   ❌ 0.559 s |    0.787 s |  0.00% |      27% | 0.295 GiB |
| Classic p11        |  1,500 |     0.192 s |    0.279 s |  0.00% |      26% | 0.132 GiB |
| Classic p11        |  1,750 |   ❌ 0.721 s |    0.861 s |  0.00% |      27% | 0.145 GiB |
| Worker  p11        |  5,250 |     0.433 s |    0.644 s |  0.00% |      27% | 0.312 GiB |
| Worker  p11        |  5,500 |   ❌ 0.548 s |    0.773 s |  0.00% |      27% | 0.315 GiB |
| Classic p12        |  1,500 |     0.237 s |    0.356 s |  0.00% |      26% | 0.133 GiB |
| Classic p12        |  1,750 |   ❌ 0.948 s |  ❌ 1.170 s |  0.00% |      26% | 0.160 GiB |
| Worker  p12        |  5,250 |     0.316 s |    0.455 s |  0.00% |      27% | 0.303 GiB |
| Worker  p12        |  5,500 |   ❌ 0.539 s |    0.697 s |  0.00% |      27% | 0.315 GiB |
