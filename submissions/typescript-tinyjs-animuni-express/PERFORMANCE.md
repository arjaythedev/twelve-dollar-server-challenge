# 100K investigation (15 November 2026)

100,000 users and a peak RSS below 300 MB remain targets, not verified results.
The official benchmark machine is unavailable to this development session.

## Official baseline

[PR #410's benchmark report](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/410#issuecomment-6878634773)
at `048312f0a5c1eb9580856076c5d9b37bf7756e50`:

| Five-minute users | Requests/s (whole run / hold) | p95 / p99 | Errors | CPU | Peak memory |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 71,000 | 6,534 / 6,985 | 184.3 / 524 ms | 0.93% | 92% (132 us/request) | 219 MB |

75,000 failed its five-minute hold with 1.59% errors; 78,000 failed its
two-minute probe with 1.36%. The report attributes errors to connect timeouts
past the service's 65,535-descriptor limit. Correctness was 42/42.

## Bottlenecks and boundaries

- **Descriptors/network:** `SPEC.md` grants `LimitNOFILE=65535`; keeping every
  connection idle for 120 seconds cannot accommodate 100,000 simultaneous
  sockets. A VU can remain active while its idle connection closes and is
  reopened. This change reclaims idle direct connections after three seconds,
  preserving 120 seconds for active handlers and loopback proxies. Bun's timeout
  timer is coarse; three seconds is its configured value, not an exact deadline.
  No descriptor limit, kernel setting, listen backlog or host configuration is
  raised. Changing a shell soft limit would not override systemd's hard limit.
- **CPU/event loop:** the official baseline already consumed 92% CPU. A local
  saturated 50/50 feed/post read profile (Bun `--cpu-prof`, 3,641 samples) assigned
  70.0% of self samples to SQLite `all`, 13.2% to `get`, and approximately 7.3%
  to JSON functions. Routing's individual frames were much smaller. These are
  sampled call-site costs, not a kernel profile or a separation of SQLite VM
  work from driver row allocation. Synchronous SQLite blocks the event loop;
  extra connections do not create database parallelism on a single CPU.
- **SQLite:** feed uses `posts_created_at_id_idx`; authors and individual posts
  use primary-key lookups; likes use covering `likes_post_id_idx`. No full likes
  scan or temporary feed sort appeared in the original query plan. Prepared
  statements, one connection, WAL, `synchronous=NORMAL`, foreign keys and
  immediate like transactions remain appropriate. The default page cache is
  2 MiB. No schema/index change, cache, durability reduction or speculative mmap
  increase is justified by this evidence. `busy_timeout=5000` can block the event
  loop during an external writer's lock; it is unchanged.
- **Authentication:** writes are roughly 0.17 of 2.17 requests per workload
  iteration. HMAC is verified on every authenticated request with constant-time
  comparison, canonical signature encoding, HS256, expiration and claim checks.
  Its minority workload share does not justify weakening or caching verification.
- **Memory:** the official 219 MB baseline is the only challenge-machine
  measurement. During the instrumented ten-second, 16-client read profile, RSS
  samples ranged from 49.5 to 59.2 MiB, heap-used from 1.7 to 9.2 MiB and external
  memory from 0.7 to 8.0 MiB. Profiling allocates memory itself; these numbers do
  not predict per-connection memory at 100K. Allocation rate, exact GC frequency,
  and SQLite's separate native allocation totals were not measured. No forced GC
  or enlarged page cache is introduced.

## Local connection experiment

Ubuntu under WSL2, Bun 1.4.4, checksum-verified full seed
(`738d11ab3164c13177e347f75038d1e80b43c64e754544a42d1c32bd8d7bcf1d`),
TinyJS 0.1.2 and checksum-verified k6 2.4.0. Client and server share the development
machine (WSL reports 3.7 GiB RAM and swap); this is not the 1-vCPU/2-GB/no-swap
challenge environment. Builds and load experiments run sequentially.

The unmodified official `bench/load.js` ran at 500 VUs with a 15-second ramp,
90-second hold and its normal ramp-down. Only the server process was given a
deliberately reduced soft/hard limit of 256 FDs. This reproduces descriptor
pressure without allocating 100,000 local VUs. It establishes no scaled capacity
claim. Both databases started from fresh seed copies.

| Idle timeout | Requests / average req/s | p50 / p95 / p99 ms | Errors | Peak sampled FDs | Peak RSS KiB | Average server CPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline 120 s | 3,721 / 27.43 | 0.503 / 55,285 / 55,286 | 6.82% (254 request timeouts) | 256 | 40,220 | 91.8% |
| Preliminary 3 s | 6,382 / 47.81 | 0.735 / 1.605 / 5.403 | 0% | 256 | 40,156 | 3.1% |

The final active-handler-protected variant also passed a **five-minute local
hold**, using the same 500 VUs, 256-FD cap and 15-second ramp:

| Requests / average req/s | p50 / p95 / p99 ms | Errors | Peak sampled FDs | Peak RSS KiB | Average server CPU |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 17,494 / 52.96 | 0.622 / 1.342 / 8.053 | 0.0057% (1 reset) | 244 | 43,968 | 2.8% |

All three official thresholds passed (k6 exit 0). The reset occurred on a like
request; it was not retried. A close/reuse race remains possible, so this is not
a zero-error claim. Sampled ESTABLISHED connections peaked at 235 and server-port
TIME-WAIT sockets at 3,215. TIME-WAIT is kernel state, not part of process RSS.
Server CPU was 9.22 seconds over the sampled run, approximately 527 us/request
including reconnect and idle-maintenance costs at this low request rate. Do not
compare that directly with the saturated official baseline's 132 us/request.

The preliminary variant changed only `idleTimeout`; the final variant also
protects active handlers and restores the idle timeout afterward. CPU was
sampled from `/proc/PID/stat` at 100 ticks/second, excluding the load generator;
the baseline's CPU includes descriptor-exhaustion handling. These figures are
not evidence of faster SQL or normal-load CPU savings. RSS uses `/proc/PID/status`
`VmHWM`, sampled once a second; FDs use `/proc/PID/fd`.

## CPU experiments not adopted

The profile motivated an A/B/B/A feed microbenchmark: 1,000 warm-ups per path,
then 10,000 calls per round on the full seed in one stable read snapshot. The
current row query plus `JSON.stringify` used 25.93 / 23.79 CPU us per call;
SQLite `json_object`/`json_group_array` used 33.91 / 36.01. Exact bytes matched on
the full seed and empty/Unicode/control-character/tied-timestamp fixtures, but
SQL JSON was slower and its sampled RSS was higher. It was rejected. The candidate
included explicit aggregate ordering, which adds a small temporary B-tree for
the 20 output rows; [SQLite requires explicit aggregate ORDER BY for a guaranteed
order](https://www.sqlite.org/lang_aggfunc.html).

A separate 20,000-call-per-round A/B/B/A, 50/50 feed/post probe compared mmap off
with 256 MiB mmap. CPU us/call were 16.93 (off), 14.44 (on), 14.50 (on), 14.94
(off); final warm-round separation was only about 3%. That small in-process
difference does not justify changing the memory footprint without an HTTP and
challenge-machine benefit. mmap and page-cache configuration remain unchanged.

## Regression coverage and reproduction

From a clean Linux checkout, with the pinned Bun installed:

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/typescript-tinyjs-animuni-express
cd submissions/typescript-tinyjs-animuni-express
bun smoke.ts transport.ts
bun smoke.ts
```

The official runner uses loopback. To check short-idle direct mode, start
`transport.ts` with `HOST=0.0.0.0`, a fresh seed copy in `SQLITE_PATH`, the challenge
`JWT_SECRET` and an unused `PORT`, then run unchanged `test/test.sh` against it.
Both loopback and direct mode pass 42/42 locally. Smoke additionally covers HEAD,
method rejection, authentication failures, malformed JSON, concurrent likes and
posts, restart persistence, missing-database health, six-second uploads, writes
around idle expiry, idle reclamation, loopback reuse, and an upload completing
through SIGTERM and persisting after restart. No POST retry is added.

To reproduce descriptor pressure, build once, start on a disposable seed copy:

```bash
(ulimit -n 256; exec env HOST=0.0.0.0 PORT=3099 \
  SQLITE_PATH=/absolute/path/to/disposable.db \
  JWT_SECRET=twelve-dollar-challenge bun transport.ts)
```

In a separate terminal at repository root:

```bash
k6 run -e BASE_URL=http://127.0.0.1:3099 -e VUS=500 \
  -e RAMP_UP=15s -e DURATION=90s --summary-export=/tmp/result.json bench/load.js
```

For the baseline use the submission from the parent commit, its own build and a
separate fresh database. Do not impose the diagnostic 256-FD limit on an official
run. Record actual `/proc/PID/limits`, CPU ticks, RSS and FD counts; use
`ss -Htan 'sport = :3099'` to distinguish ESTABLISHED and TIME-WAIT sockets.

## Remaining official validation

No new official benchmark has run. The 50K, 60K, 71K, 75K, 80K, 90K and 100K
progression (then 110K if 100K succeeds) is pending on the challenge machine.
Use unchanged workload/scoring, fresh data, the normal 60-second ramp, and a
five-minute confirmation. Record hold duration, req/s, p50/p95/p99, errors and
categories, CPU, peak RSS and FD counts. The thresholds remain p95 <500 ms,
p99 <1 second and <1% errors.

Shorter keep-alive trades sockets for reconnects and TIME-WAIT state; reconnect
CPU, kernel socket memory, timeout races and the single-core SQLite ceiling can
still prevent 100K or 300 MB. The accepted backlog and host/container limits
cannot be inspected remotely from this session. The legitimate fallback is the
highest passing official hold, not raised resource limits or extrapolation.
