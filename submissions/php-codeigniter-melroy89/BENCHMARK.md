# Local qualification and tuning

These are local measurements, **not an official challenge score or a proven maximum**.
The DigitalOcean droplet has a different CPU and storage subsystem.

## Environment

- Host: Linux x86_64, AMD Ryzen Threadripper 7960X.
- Server: Ubuntu 24.04 container, non-root UID 1000, **1 CPU quota, 2 GiB RAM, zero swap**,
  open-file limit 65,535. No kernel tuning or system service configuration changes.
- Runtime: CodeIgniter 4.7.5, FrankenPHP 1.13.0, PHP 8.5.11 ZTS, SQLite 3.45.2.
- Load generator: separate k6 1.4.2 container on the same host, outside the server's CPU/RAM limits.
  The generator needs several GiB at high VU counts; its memory is not counted as server memory.
- Data: the repository's deterministic full seed, content SHA-256
  `738d11ab3164c13177e347f75038d1e80b43c64e754544a42d1c32bd8d7bcf1d`.
  Every capacity trial starts with a fresh copy. Schema and indexes remain unchanged.
- Repository base: `026bdb68d94d0d932bb5981321ad82c70618d752`, shared by the fork and upstream
  at qualification time on 2026-10-13. Application-source checksums are recorded in `benchmarks/results.json`.

## Feed diagnostics

Each retained trial used the same seed and `ab -k -c 32 -t 15 http://SERVER:3000/feed`.
All trials below completed with zero failed requests. These short read-only diagnostics
compare implementation costs; they do not measure a challenge user score or establish statistical significance.

| Configuration | Requests/s | p99 |
| --- | ---: | ---: |
| Initial CI4 worker, new PDO connection and SQL preparation each request | 901 | 77 ms |
| Persistent PDO connection and prepared statements | 1,450 | 70 ms |
| Plus tracing JIT, direct worker routing and Go tuning; forced per-request GC | 1,665 | 55 ms |
| Same, automatic PHP GC; two workers | 1,714 | 29 ms |
| Same, one worker | 1,653 | 37 ms |
| Same, four workers | 1,617 | 27 ms |
| Two workers, unused module discovery and controller attributes disabled | 1,958 | 24 ms |
| Final: JSON encoded once, passed through CI4's JSON response | 1,989 | 26 ms |

The final diagnostic has **2.21×** the initial throughput. The small last-step difference is
within possible run-to-run variation. Two workers were retained for their higher throughput;
request-specific services still follow the official CI4 reset lifecycle. PHP's automatic GC
remains enabled. No application data or JWT verification results are cached.

## Challenge request mix

All capacity runs use the repository's **unmodified `bench/load.js`**, including its read/like/post
mix, think times, keep-alive behavior and original thresholds: p95 < 500 ms, p99 < 1,000 ms,
and HTTP failure rate < 1%.

| Candidate | Users | Hold | p95 | p99 | Failed HTTP requests | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Persistent PDO, before final runtime/routing tuning | 10,000 | 60 s | 2.01 ms | 17.59 ms | 0 / 98,428 | Pass |
| Intermediate runtime tuning | 20,000 | 60 s | 4,600 ms | 4,760 ms | 0 / 172,414 | Fail: latency |
| Final candidate | 15,000 | 60 s | 2.68 ms | 178.98 ms | 0 / 155,656 | Pass |
| Final candidate | 17,500 | 60 s | 8.68 ms | 45.34 ms | 0 / 181,391 | Pass |
| Final candidate | 20,000 | 60 s | 394.88 ms | 489.38 ms | 0 / 205,726 | Pass |
| Final candidate | **18,500** | **5 min** | **138.94 ms** | **258.74 ms** | **0 / 689,076** | **Pass** |
| Final candidate | 20,000 | **5 min** | 1549.71 ms | 1828.07 ms | 0 / 726,596 | **Fail: latency** |

The verified local result is **18,500 users for a five-minute hold**, after a two-minute
1,000-user warm-up, with the original 60-second ramp-up and 30-second ramp-down. Peak
process RSS sampled every five seconds was **877.2 MiB**; no out-of-memory events occurred.
20,000 users passed a short diagnostic but failed the full hold, so it is not a passing score.
The exact maximum was not narrowed to 250-user increments; this is a selected passing load.
Short trials are diagnostics. Full k6 JSON summaries are retained in `benchmarks/`.
The 10,000-user run also included a small supplementary correctness-check workload and its
summary-export step hit a file-permission error; the retained console metrics passed, but it
is not treated as a clean scoring run. Later exports run with the correct output-directory UID.

## Correctness evidence

- Final `install.sh` completed in a fresh Ubuntu 24.04 container.
- Final `test/run.sh`: **42 passed, zero failed**, as a non-root user.
- `checks/worker.py`: passed request identity isolation, fresh JWT checks, Unicode limits,
  64 concurrent duplicate-like requests, cross-worker visibility and failed-write recovery.
- Forced-kill durability check: all **128 acknowledged posts and 128 acknowledged likes**
  remained after `SIGKILL`; `PRAGMA integrity_check` returned `ok`; `sqlite_master` matched the seed.
- The original HTTP/1.1 client socket was reused successfully after **66 seconds idle**.
- Composer production audit reported no known security advisories. Shell syntax and PHP lint passed.
- Exact-commit remote CI is tracked by the pull request. Official droplet qualification has not been run.

## Reproduce the load

Install k6 on a separate load-generator machine or container. Start this submission against a
fresh seed copy, then run from the repository root:

```sh
# Warm-up: 1,000 users with a two-minute hold.
k6 run -e BASE_URL=http://SERVER:3000 -e VUS=1000 -e RAMP_UP=1s -e DURATION=2m bench/load.js

# Confirmation: substitute the chosen user count, retaining the default 60 s ramp-up,
# five-minute hold and 30 s ramp-down.
k6 run --summary-export=ci4-summary.json -e BASE_URL=http://SERVER:3000 -e VUS=18500 bench/load.js
```

Do not run correctness checks concurrently with a scored trial: they write data and add load.
Use a fresh database when changing candidates or user counts. Restart the process after changing
PHP/Caddy settings; production OPcache does not reload source files automatically.
