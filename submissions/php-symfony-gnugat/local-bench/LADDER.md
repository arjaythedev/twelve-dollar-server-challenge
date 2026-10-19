# The ladder

How many users the app holds, one optimisation at a time, in both FrankenPHP modes: classic (Symfony boots on every
request, like PHP-FPM) and worker (Symfony boots once per worker). One commit per phase, each with its table.

**Setup**: `local-bench/bench.sh` runs the server in an Ubuntu 24.04 container pinned to one core of an Apple silicon
Mac (OrbStack), with 2 GB and no swap; k6 1.4.0 runs `bench/load.js` in its own containers on the other 14 cores.
Like the real score: a 1,000-user 2 min warm-up (not measured), 60 s ramp-up, 5 min hold, 30 s ramp-down; a run
passes with p95 < 0.5 s, p99 < 1 s and < 1% errors. `find-limit.sh` steps up, then bisects to 250 users: each table
shows the last passing run and the first failing one. Hold CPU is the server's median CPU during the hold.
Measured 2026-10-16 to 2026-10-19; phase 0 measured at the start and the end gives the drift: one step (250 users).

**Only compare these numbers with each other**: one core here is much faster than the droplet's vCPU, and the
droplet estimate below is rough.

**Open files**: the rules give the server 65,535 open files, one per keep-alive connection (one per user), so from
phase 6 worker mode stops at ~65,500 users whatever its CPU. The worker rows from phase 6 are measured with 200,000
open files (`NOFILE=200000`), to see where CPU or memory stops it; the summary gives both up to phase 12, and only
the 200,000-file limit from phase 13.

## Summary

| Phase | Change | Classic | Worker | Worker, 65,535 files | Helps |
|---|---|--:|--:|--:|---|
| 0 | Idiomatic Symfony 8.1, FrankenPHP 1.13, PHP 8.5 ZTS, defaults | 8,500 | 23,000 | | |
| 1 | php.ini production settings (OPcache, realpath) | 8,500 | 23,000 | | none |
| 2 | Preloading | 9,750 | 23,500 | | classic |
| 3 | `classmap-authoritative` | 9,750 | 23,500 | | none |
| 4 | JIT (tracing) | 11,000 | 33,000 | | both |
| 5 | DQL read model | 13,500 | 52,000 | | both |
| 6 | JsonStreamer | 15,750 | 65,500 | 65,500 | both |
| 7 | DBAL read model | 15,250 | 72,750 | 65,500 | both (classic: +9% requests at saturation) |
| 8 | Firewall on POST /posts only | 17,250 | 73,250 | 65,500 | classic |
| 9 | API-only routing in the Caddyfile | 17,250 | 75,750 | 65,500 | worker |
| 10 | Prepared-statement reuse | 17,000 | 76,750 | 66,500 | worker |
| 11 | Worker mode by default | 17,250 | 76,750 | 65,750 | |
| 12 | SQLite writes the reads' JSON | 17,500 | 77,500 | 66,000 | both, a little |
| 13 | Persistent SQLite connections | 29,500 | 77,500 | | classic |
| 14 | DBAL writes | 29,500 | 79,000 | | worker (less memory) |
| 15 | `max_threads 8` | 30,250 | 79,500 | | both, within noise |

Experiments, not kept: classic `num_threads 8` after phase 4 (11,000 -> 8,750); `zend.enable_gc=0` after phase 9
(classic 17,500, worker 78,000: within a step or two); `GOMEMLIMIT=1100MiB` after phase 9 (worker 65,500 -> 51,500:
FrankenPHP already sets it to 90% of the container's memory, 1.8 GiB, when it's unset); on phase 12, never restarting
workers (78,000), 1 worker (74,500), 4 workers (77,250); `GOGC=200` after phase 15 (classic 30,500 for 0.4 GiB more
memory, worker fails at 79,000).

What stops each mode: classic mode is CPU-bound up to phase 6, then fails at 80-85% CPU up to phase 12: its PHP
threads were waiting on opening the database on every request, and with persistent connections (phase 13) it is
CPU-bound again, the core full; worker mode is CPU-bound up to phase 5, then memory-bound (the 2 GB are full at its
limit).

## All runs

|                      |  Users | p95 < 0.5 s |   p99 < 1 s |    Errors | Hold CPU |  Peak RAM |
|----------------------|-------:|------------:|------------:|----------:|---------:|----------:|
| Classic p0           |  8,500 |     0.336 s |     0.384 s |     0.00% |      95% | 0.424 GiB |
| Classic p0           |  8,750 |  ❌ 0.556 s |     0.589 s |     0.00% |      96% | 0.439 GiB |
| Worker  p0           | 23,000 |     0.437 s |     0.482 s |     0.00% |     108% | 1.081 GiB |
| Worker  p0           | 23,250 |  ❌ 0.515 s |     0.576 s |     0.00% |     108% | 1.099 GiB |
| Classic p1           |  8,500 |     0.278 s |     0.312 s |     0.00% |      95% | 0.423 GiB |
| Classic p1           |  8,750 |  ❌ 0.559 s |     0.596 s |     0.00% |      95% | 0.441 GiB |
| Worker  p1           | 23,000 |     0.471 s |     0.498 s |     0.00% |     108% | 1.086 GiB |
| Worker  p1           | 23,250 |  ❌ 0.593 s |     0.649 s |     0.00% |     108% | 1.107 GiB |
| Classic p2           |  9,750 |     0.402 s |     0.454 s |     0.00% |      95% | 0.497 GiB |
| Classic p2           | 10,000 |  ❌ 0.696 s |     0.735 s |     0.00% |      95% | 0.519 GiB |
| Worker  p2           | 23,500 |     0.481 s |     0.524 s |     0.00% |     108% | 1.128 GiB |
| Worker  p2           | 23,750 |  ❌ 0.598 s |     0.649 s |     0.00% |     108% | 1.152 GiB |
| Classic p3           |  9,750 |     0.432 s |     0.538 s |     0.00% |      94% | 0.501 GiB |
| Classic p3           | 10,000 |  ❌ 0.612 s |     0.649 s |     0.00% |      95% | 0.516 GiB |
| Worker  p3           | 23,500 |     0.467 s |     0.528 s |     0.00% |     108% | 1.121 GiB |
| Worker  p3           | 23,750 |  ❌ 0.611 s |     0.652 s |     0.00% |     108% | 1.143 GiB |
| Classic p4           | 11,000 |     0.446 s |     0.506 s |     0.00% |      94% | 0.566 GiB |
| Classic p4           | 11,250 |  ❌ 0.718 s |     0.751 s |     0.00% |      93% | 0.587 GiB |
| Worker  p4           | 33,000 |     0.461 s |     0.503 s |     0.00% |     108% | 1.562 GiB |
| Worker  p4           | 33,250 |  ❌ 0.648 s |     0.696 s |     0.00% |     108% | 1.585 GiB |
| Classic threads8 ✗   |  8,750 |     0.299 s |     0.471 s |     0.00% |      77% | 0.470 GiB |
| Classic threads8 ✗   |  9,000 |  ❌ 0.528 s |     0.661 s |     0.00% |      79% | 0.491 GiB |
| Classic p5           | 13,500 |     0.455 s |     0.505 s |     0.00% |      90% | 0.675 GiB |
| Classic p5           | 13,750 |  ❌ 0.729 s |     0.916 s |     0.00% |      91% | 0.706 GiB |
| Worker  p5           | 52,000 |     0.405 s |     0.581 s |     0.00% |     109% | 1.899 GiB |
| Worker  p5           | 52,250 |  ❌ 0.652 s |     0.763 s |     0.00% |     109% | 1.912 GiB |
| Classic p6           | 15,750 |     0.423 s |     0.607 s |     0.00% |      88% | 0.780 GiB |
| Classic p6           | 16,000 |  ❌ 0.588 s |  ❌ 1.260 s |     0.00% |      87% | 0.831 GiB |
| Worker  p6           | 65,500 |     0.418 s |     0.496 s |     0.00% |     115% | 1.894 GiB |
| Worker  p6           | 65,750 |  ❌ 0.579 s |     0.673 s |     0.00% |     115% | 1.885 GiB |
| Classic p7           | 15,250 |     0.158 s |     0.352 s |     0.00% |      79% | 0.735 GiB |
| Classic p7           | 15,500 |     0.431 s |  ❌ 1.650 s |     0.00% |      80% | 0.840 GiB |
| Worker  p7           | 72,750 |     0.378 s |     0.596 s |     0.00% |     114% | 1.907 GiB |
| Worker  p7           | 73,000 |  ❌ 0.669 s |  ❌ 1.640 s |  ❌ 6.18% |     114% | 1.913 GiB |
| Classic p8           | 17,250 |     0.272 s |     0.490 s |     0.00% |      84% | 0.842 GiB |
| Classic p8           | 17,500 |  ❌ 0.628 s |  ❌ 1.880 s |     0.00% |      84% | 0.929 GiB |
| Worker  p8           | 73,250 |     0.322 s |     0.477 s |     0.00% |     113% | 1.893 GiB |
| Worker  p8           | 73,500 |  ❌ 1.010 s | ❌ 60.000 s |  ❌ 3.40% |     114% | 1.998 GiB |
| Classic p9           | 17,250 |     0.227 s |     0.627 s |     0.00% |      83% | 0.840 GiB |
| Classic p9           | 17,500 |  ❌ 0.802 s |  ❌ 2.170 s |     0.00% |      83% | 0.954 GiB |
| Worker  p9           | 75,750 |     0.440 s |     0.861 s |     0.00% |     114% | 1.996 GiB |
| Worker  p9           | 76,000 |  ❌ 0.666 s |  ❌ 1.040 s |     0.60% |     113% | 1.906 GiB |
| Classic no GC        | 17,500 |     0.426 s |     0.620 s |     0.00% |      84% | 0.856 GiB |
| Classic no GC        | 17,750 |  ❌ 0.557 s |  ❌ 1.120 s |     0.00% |      84% | 0.897 GiB |
| Worker  no GC        | 78,000 |     0.228 s |     0.419 s |     0.00% |     110% | 1.914 GiB |
| Worker  no GC        | 78,250 |     0.092 s |  ❌ 2.040 s | ❌ 82.26% |      75% | 1.902 GiB |
| Classic GOMEMLIMIT ✗ | 17,250 |     0.493 s |     0.981 s |     0.00% |      83% | 0.874 GiB |
| Classic GOMEMLIMIT ✗ | 17,500 |     0.341 s |  ❌ 1.110 s |     0.00% |      84% | 0.884 GiB |
| Worker  GOMEMLIMIT ✗ | 51,500 |     0.074 s |     0.111 s |     0.00% |      91% | 1.164 GiB |
| Worker  GOMEMLIMIT ✗ | 51,750 |  ❌ 1.620 s |  ❌ 2.260 s |     0.00% |     105% | 1.486 GiB |
| Classic p10          | 17,000 |     0.317 s |     0.670 s |     0.00% |      82% | 0.837 GiB |
| Classic p10          | 17,250 |  ❌ 0.544 s |  ❌ 1.520 s |     0.00% |      84% | 0.879 GiB |
| Worker  p10          | 76,750 |     0.314 s |     0.487 s |     0.00% |     112% | 1.915 GiB |
| Worker  p10          | 77,000 |  ❌ 0.697 s | ❌ 60.000 s |  ❌ 7.34% |     112% | 1.998 GiB |
| Classic p11          | 17,250 |     0.248 s |     0.476 s |     0.00% |      83% | 0.842 GiB |
| Classic p11          | 17,500 |  ❌ 0.524 s |  ❌ 1.310 s |     0.00% |      84% | 0.889 GiB |
| Worker  p11          | 76,750 |     0.259 s |     0.361 s |     0.00% |     111% | 1.889 GiB |
| Worker  p11          | 77,000 |  ❌ 0.791 s | ❌ 60.000 s |  ❌ 4.61% |     113% | 1.998 GiB |
| Classic p12          | 17,500 |     0.319 s |     0.998 s |     0.00% |      84% | 0.872 GiB |
| Classic p12          | 17,750 |  ❌ 0.578 s |  ❌ 1.200 s |     0.00% |      84% | 0.906 GiB |
| Worker  p12          | 77,500 |     0.321 s |     0.682 s |     0.11% |     110% | 1.998 GiB |
| Worker  p12          | 77,750 |  ❌ 0.653 s | ❌ 42.160 s | ❌ 46.09% |     112% | 1.997 GiB |
| Classic p13          | 29,500 |     0.499 s |     0.542 s |     0.00% |     107% | 1.424 GiB |
| Classic p13          | 29,750 |  ❌ 0.743 s |     0.830 s |     0.00% |     108% | 1.457 GiB |
| Worker  p13          | 77,500 |     0.363 s |     0.744 s |     0.00% |     110% | 1.996 GiB |
| Worker  p13          | 77,750 |     0.477 s | ❌ 60.000 s | ❌ 41.70% |     110% | 1.997 GiB |
| Classic p14          | 29,500 |     0.277 s |     0.314 s |     0.00% |     108% | 1.402 GiB |
| Classic p14          | 29,750 |  ❌ 0.614 s |     0.657 s |     0.00% |     108% | 1.443 GiB |
| Worker  p14          | 79,000 |     0.180 s |     0.236 s |     0.00% |     107% | 1.898 GiB |
| Worker  p14          | 79,250 |  ❌ 0.793 s | ❌ 60.000 s | ❌ 81.80% |     105% | 1.997 GiB |
| Classic p15          | 30,250 |     0.456 s |     0.554 s |     0.00% |     108% | 1.480 GiB |
| Classic p15          | 30,500 |  ❌ 0.560 s |     0.625 s |     0.00% |     108% | 1.495 GiB |
| Worker  p15          | 79,500 |     0.382 s |     0.492 s |     0.00% |     110% | 1.904 GiB |
| Worker  p15          | 79,750 |     0.425 s | ❌ 60.000 s | ❌ 60.71% |     108% | 1.998 GiB |
| Classic GOGC=200 ✗   | 30,500 |     0.478 s |     0.540 s |     0.00% |     107% | 1.904 GiB |
| Classic GOGC=200 ✗   | 30,750 |  ❌ 0.661 s | ❌ 17.010 s |     0.00% |     108% | 1.992 GiB |
| Worker  GOGC=200 ✗   | 79,000 |     0.001 s | ❌ 60.000 s | ❌ 92.28% |     112% | 1.998 GiB |

Worker mode with the rules' 65,535 open files:

|                            |  Users | p95 < 0.5 s |   p99 < 1 s |    Errors | Hold CPU |  Peak RAM |
|----------------------------|-------:|------------:|------------:|----------:|---------:|----------:|
| Worker  p12 65,535 files   | 66,000 |     0.047 s |     0.083 s |     0.90% |      89% | 1.897 GiB |
| Worker  p12 65,535 files   | 66,250 |  ❌ 3.940 s | ❌ 36.480 s | ❌ 71.72% |      93% | 1.929 GiB |
| Worker  p6 65,535 files    | 65,500 |     0.409 s |     0.522 s |     0.00% |     115% | 1.882 GiB |
| Worker  p6 65,535 files    | 65,750 |     0.027 s |     0.125 s | ❌ 93.70% |      77% | 1.888 GiB |
| Worker  p7 65,535 files    | 65,500 |     0.111 s |     0.142 s |     0.00% |     101% | 1.883 GiB |
| Worker  p7 65,535 files    | 65,750 |     0.018 s |     0.052 s | ❌ 93.70% |      76% | 1.894 GiB |
| Worker  p8 65,535 files    | 65,500 |     0.100 s |     0.133 s |     0.00% |      99% | 1.892 GiB |
| Worker  p8 65,535 files    | 65,750 |     0.020 s |     0.052 s | ❌ 93.71% |      76% | 1.881 GiB |
| Worker  p9 65,535 files    | 65,500 |     0.081 s |     0.116 s |     0.00% |      95% | 1.892 GiB |
| Worker  p9 65,535 files    | 65,750 |  ❌ 3.370 s | ❌ 34.710 s | ❌ 71.82% |      96% | 1.935 GiB |
| Worker  no GC 65,535 files | 65,500 |     0.053 s |     0.087 s |     0.00% |      86% | 1.893 GiB |
| Worker  no GC 65,535 files | 65,750 |  ❌ 4.050 s | ❌ 34.320 s | ❌ 77.29% |      97% | 1.917 GiB |
| Worker  p10 65,535 files   | 66,500 |     0.059 s |     0.094 s |     0.98% |      92% | 1.889 GiB |
| Worker  p10 65,535 files   | 66,750 |  ❌ 3.510 s | ❌ 35.740 s | ❌ 78.39% |      89% | 1.969 GiB |
| Worker  p11 65,535 files   | 65,750 |     0.064 s |     0.104 s |     0.85% |      92% | 1.894 GiB |
| Worker  p11 65,535 files   | 66,000 |  ❌ 3.050 s | ❌ 60.000 s | ❌ 71.07% |     102% | 1.998 GiB |

Phase 12's worker experiments:

|                         |  Users | p95 < 0.5 s |   p99 < 1 s |    Errors | Hold CPU |  Peak RAM |
|-------------------------|-------:|------------:|------------:|----------:|---------:|----------:|
| Worker  never restart ✗ | 78,000 |     0.205 s |     0.265 s |     0.00% |     110% | 1.905 GiB |
| Worker  never restart ✗ | 78,250 |     0.476 s |     0.853 s | ❌ 54.39% |     112% | 1.995 GiB |
| Worker  1 worker ✗      | 74,500 |     0.358 s |     0.680 s |     0.00% |     104% | 1.900 GiB |
| Worker  1 worker ✗      | 74,750 |  ❌ 0.740 s | ❌ 60.000 s |  ❌ 3.11% |     105% | 1.998 GiB |
| Worker  4 workers ✗     | 77,250 |     0.225 s |     0.285 s |     0.00% |     111% | 1.913 GiB |
| Worker  4 workers ✗     | 77,500 |     0.492 s | ❌ 60.000 s |  ❌ 1.31% |     113% | 1.997 GiB |

## Peers, same setup

Two submissions with droplet scores, run here from copies of their folders. Above 64,000 users the load is split
across two or three k6 containers (one runs out of ports near 64,500 connections), with 200,000 open files.

| Peer | Droplet | Here | Stops here on |
|---|--:|--:|---|
| PR 10, Python FastAPI | 22,000 (91% CPU) | 146,250 | CPU (94%) |
| PR 24, PHP Swoole | 40,000 (77% CPU, fails on p99) | 68,750 | its own `max_connection` 65,535 |
| PR 24, `max_connection` 200,000 (local change) | | 82,750 | memory (2 GB) |

|                                  |   Users | p95 < 0.5 s |   p99 < 1 s |   Errors | Hold CPU |  Peak RAM |
|----------------------------------|--------:|------------:|------------:|---------:|---------:|----------:|
| Python PR 10                     | 146,250 |     0.116 s |     0.963 s |    0.00% |      94% | 1.250 GiB |
| Python PR 10                     | 146,500 |  ❌ 1.180 s |  ❌ 2.440 s |    0.01% |      95% | 1.306 GiB |
| Python PR 10 fixed               |  22,000 |     0.003 s |     0.032 s |    0.00% |      49% | 0.234 GiB |
| Swoole PR 24                     |  68,750 |     0.010 s |     0.030 s |    0.96% |      41% | 1.592 GiB |
| Swoole PR 24                     |  69,000 |     0.011 s |     0.030 s | ❌ 1.05% |      42% | 1.587 GiB |
| Swoole PR 24 max_connection 200k |  82,750 |     0.163 s |     0.837 s |    0.00% |      53% | 1.998 GiB |
| Swoole PR 24 max_connection 200k |  83,000 | ❌ 37.250 s | ❌ 60.000 s | ❌ 8.61% |      59% | 1.998 GiB |
| Swoole PR 24 fixed               |  40,000 |     0.004 s |     0.034 s |    0.00% |      39% | 0.984 GiB |

## Droplet estimate

Rough, a range rather than a number. At the same load, a local core does the work of about 1.9 droplet vCPUs: PR 10
uses 91% CPU on the droplet at 22,000 users and 49% here (1.85); PR 24, 77% and 39% at 40,000 (1.95). But local CPU
doesn't grow in step with the users: PR 10 goes from 49% at 22,000 users to only 94% at 146,250, and our phases use
50-57% already at a few thousand users (see below). Likely causes: the Mac's core clocks down when lightly loaded,
and the kernel's network work isn't all charged to the server's core here, while it is on the droplet. So the local
limit divided by 1.9 overestimates the droplet, and PR 10's ratio of limits (146,250 / 22,000 = 6.6) can't be applied
as is either: its requests are light, so network overhead weighs more in its cost than in PHP's.

Two bounds, then:
- **high**: local limit / 1.9, as if the droplet scaled like the Mac at saturation;
- **low**: as if PHP behaved like PR 10. CPU added per 1,000 users here, between the lowest and highest fixed load
  below, times PR 10's droplet/local ratio for it (4.14% / 0.36% = 11.4), up to PR 10's 91%.

| | Local limit | Local CPU per 1,000 users | Low | High |
|---|--:|--:|--:|--:|
| Phase 0, classic | 8,500 | 4.8% | ~1,700 | ~4,500 |
| Phase 0, worker | 23,000 | 1.5% | ~5,200 | ~12,100 |
| Phase 12, classic | 17,500 | 1.5% | ~5,200 | ~9,200 |
| Phase 12, worker | 77,500 | 0.65% | ~12,200 | ~40,800 |
| Phase 15, classic | 30,250 | not measured | | ~15,900 |
| Phase 15, worker | 79,500 | not measured | | ~41,800 |

Phase 15's low bound needs its fixed loads, not run. The real number needs the real box: the official run when this is submitted.

Fixed loads, phases 0 and 12:

|                   |  Users | p95 < 0.5 s | p99 < 1 s | Errors | Hold CPU |  Peak RAM |
|-------------------|-------:|------------:|----------:|-------:|---------:|----------:|
| Classic p0 fixed  |  2,500 |     0.011 s |   0.016 s |  0.00% |      51% | 0.154 GiB |
| Classic p0 fixed  |  4,000 |     0.009 s |   0.014 s |  0.00% |      55% | 0.218 GiB |
| Classic p0 fixed  |  5,500 |     0.010 s |   0.016 s |  0.00% |      65% | 0.282 GiB |
| Worker  p0 fixed  |  6,000 |     0.006 s |   0.010 s |  0.00% |      57% | 0.305 GiB |
| Worker  p0 fixed  | 10,000 |     0.005 s |   0.008 s |  0.00% |      61% | 0.478 GiB |
| Worker  p0 fixed  | 14,000 |     0.006 s |   0.009 s |  0.00% |      69% | 0.653 GiB |
| Classic p12 fixed |  6,000 |     0.010 s |   0.016 s |  0.00% |      54% | 0.335 GiB |
| Classic p12 fixed |  9,000 |     0.009 s |   0.016 s |  0.00% |      56% | 0.467 GiB |
| Classic p12 fixed | 12,000 |     0.011 s |   0.027 s |  0.00% |      64% | 0.601 GiB |
| Worker  p12 fixed |  8,000 |     0.005 s |   0.007 s |  0.00% |      51% | 0.429 GiB |
| Worker  p12 fixed | 12,000 |     0.005 s |   0.007 s |  0.00% |      53% | 0.604 GiB |
| Worker  p12 fixed | 16,000 |     0.005 s |   0.008 s |  0.00% |      56% | 0.782 GiB |
