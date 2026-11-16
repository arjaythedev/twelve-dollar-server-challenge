# Capacity investigation (16 November 2026)

The last verified challenge score is **71,000 users for five minutes**.
More than 100,000 users and peak RSS below 300 MB remain targets.

## Official evidence and the regression

[PR #410's report](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/410#issuecomment-6878634773)
at `048312f`, direct on the 1-vCPU/2-GB box:

| Users / hold | Requests/s overall / hold | p95 / p99 | Errors | CPU | Peak memory |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 71,000 / 5 minutes | 6,534 / 6,985 | 184.3 / 524 ms | 0.93% | 92% (132 us/request) | 219 MB |

75,000 failed its five-minute hold with 1.59% errors; 78,000 failed its
two-minute probe with 1.36%. Errors were connect timeouts at the fixed
65,535-descriptor ceiling. Correctness was 42/42.

The subsequent three-second idle timeout at `65b50f0` was a regression.
[PR #467's official report](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/467#issuecomment-6898503703)
found **score 0**, with approximately 7% errors from 20,000 down to 500 users.
At 500 users, all 7,601 reads succeeded, but 524/542 likes and 60/72 creates
failed, almost all with EOF. Short silent closure invalidated clients' idle
connections; GET recovery did not protect non-retryable POSTs.

The earlier local 500-VU/256-FD experiment had passed a five-minute hold with
one reset (0.0057%). Its success did **not** predict the official client/network
behavior. That experiment is rejected as evidence for deploying short idle
timeouts. The merged leaderboard entry remains at 71,000.

## Revised connection policy

- Restore the proven 120-second idle timeout for every connection. Remove the
  per-request timeout overrides.
- On Linux direct deployments, read the process's soft `Max open files` limit
  from `/proc/self/limits`. Once per second, count `/proc/self/fd` entries.
- At 75% of that limit, set `Connection: close` on completed responses,
  including errors. Bun sends the complete body before closing the socket;
  the client learns retirement before its next request.
- Resume normal keep-alive below 50% of the limit. Hysteresis avoids toggling
  at every sample. Unreference the timer and clear it on graceful shutdown.
- Preserve loopback proxy connections. Do not change resource limits, kernel
  settings, listen backlog or Nginx configuration.

This policy applies to any direct workload; it does not inspect user identity,
endpoint mix or benchmark metadata. No POST is retried. Sampling leaves
headroom but cannot guarantee against arbitrary bursts between samples.
Inspection failures conservatively enable response retirement and are logged.

## Local pressure diagnostic

On 16 November, unchanged `bench/load.js` ran with checksum-verified k6 2.4.0,
500 VUs, a 15-second ramp and 90-second hold on a fresh checksum-verified full
seed. Only the server had its FD limit reduced to 256. Client and server shared
WSL2 (3.7 GiB RAM and swap), rather than the challenge environment.

The revised policy passed every threshold, k6 exit 0: **6,368 requests, zero
errors**, 45.33 requests/s overall, median 0.596 ms, p95 1.08 ms and p99 2.12 ms.
Once-per-second resource samples peaked at 233 descriptors and 42,552 KiB RSS.
Server CPU ticks increased 2.99 seconds over 140 sampled seconds (2.14% of a
CPU, excluding the client; `/proc` clock resolution 100 ticks/second).
This tests descriptor pressure and writes, not scaled user capacity.

The same setup also passed a **five-minute hold**: **17,485 requests, zero
errors**, 51.25 requests/s overall, median 0.581 ms, p95 1.00 ms and p99
1.43 ms. Sampled descriptors peaked at 235 and sampled RSS at 50,036 KiB.
Server CPU ticks increased 7.34 seconds over 342 sampled seconds (2.15% of a
CPU). This is a separate fresh database; all official thresholds passed,
k6 exit 0. These measurements still do not establish remote capacity.

A separate probe held 49,152 `/dev/null` descriptors and sampled the same
directory ten times at one-second intervals. Enumeration took 21.9-31.0 ms
after an initial 78.5-ms sample, averaging 29.7 ms CPU/sample (2.97% of one CPU
at that cadence). File descriptors reproduce directory enumeration cost, not
socket memory or HTTP load. The monitor's cost is material and requires an
official capacity check; it is not claimed to be free.

## CPU, SQLite, authentication and memory

The official baseline already used 92% CPU. Earlier local Bun CPU profiling
(3,641 samples, saturated 50/50 feed/post reads) attributed about 70% of self
samples to SQLite `all`, 13.2% to `get` and 7.3% to JSON functions. These are
call-site samples, not a separation of database work from row allocation.

The feed uses `posts_created_at_id_idx`, primary-key author lookups and the
covering `likes_post_id_idx`. SQL JSON aggregation was previously slower than
row retrieval plus `JSON.stringify` (34-36 versus 24-26 CPU us/feed). A 256-MiB
mmap experiment had only about 3% separation between its final warm rounds.
Neither change was adopted.

On 16 November, a fresh full-seed read snapshot compared normal objects,
`values()` plus object mapping, and class-mapped rows, with 1,000 warmups and
20,000 calls per round in A/B/C/C/B/A order. CPU us/feed were
35.90 / 38.72 / 35.21 / 31.97 / 27.93 / 27.55. Outputs matched exactly;
warmup/order drift exceeded any consistent advantage, so neither mapping was
adopted. These are local diagnostics, not capacity estimates.

Prepared SQL, schema and indexes, per-request JWT verification, WAL,
`synchronous=NORMAL`, foreign keys and immediate like transactions remain
unchanged. No response/result/authentication cache, extra process, worker or
forced garbage collection is introduced. The official 219-MB baseline remains
the only challenge-machine memory measurement for a passing entry.

## Regression coverage

Use a clean Linux checkout and pinned Bun 1.4.4:

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/typescript-tinyjs-animuni-express
cd submissions/typescript-tinyjs-animuni-express
bun smoke.ts transport.ts
bun smoke.ts
```

The frozen-lockfile production build and all 42 official checks passed locally
on 16 November with full seed content checksum
`738d11ab3164c13177e347f75038d1e80b43c64e754544a42d1c32bd8d7bcf1d`.
Native and published Node-adapter smoke suites also passed.
`build.sh` now runs the native regressions before CI's official suite.

Native smoke checks same-socket POST reuse after 8- and 16-second gaps, slow
uploads, graceful shutdown during an upload, proxy reuse and restart
persistence. Linux smoke additionally lowers only the child server's FD limit
to 128, fills idle connections to trigger pressure, checks advertised closure,
four successful writes on new connections without replay, exact persisted
write counts and recovery to normal keep-alive. Startup reads handle split log
chunks and fail after ten seconds instead of hanging indefinitely.

## Required capacity gate

Run unchanged `bench/load.js` on the challenge-equivalent server with a separate
load generator, fresh data, normal 60-second ramp and five-minute confirmations.
First retain at least the existing 71,000-user hold, then progress through
75K/80K/90K/100K/110K. More than 100K requires a passing hold **above** 100,000.
Keep p95 <500 ms, p99 <1 second and errors <1%; record error categories, CPU,
peak RSS, FD counts and reconnects. Do not merge a lower capacity result.

No new official passing capacity is established by this revision. Response
retirement trades descriptors for reconnect CPU and TIME-WAIT state. Sampling
cost at tens of thousands of FDs, kernel socket memory and the single-core
SQLite ceiling require measurements on the actual benchmark infrastructure.
