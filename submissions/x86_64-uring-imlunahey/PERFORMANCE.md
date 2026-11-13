# Local comparison

## Five-minute confirmation: 275,000 users PASS

The FIFO candidate below passed a clean native mjc confirmation after a
1,000-user/two-minute warmup, 60-second ramp, **300.001-second full-load hold**
and normal ramp-down. No deliberate pause, ring inspector or perf profiler
was used for this confirmation.

| Users reached | Requests | p95 | p99 | HTTP/check failures | Peak server cgroup |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 275,000 | 10,267,297 | 32.489 ms | 72.998 ms | 0 / 0 | 1,385.8 MiB |

The maximum observed request latency was 555.303 ms. The sampled hold averaged
29,094 requests/s and 59.0% of one CPU. The server had one CPU, 2 GiB, no swap,
and nofile=65,535; generator limits and topology are unchanged from the earlier
native trials. There were no OOM events, shutdown exited zero, and the database
integrity check returned `ok`.

The earlier uninstrumented five-minute run failed at p99 3,543.983 ms; an
instrumented repeat failed at 1,304.078 ms. All those failures are retained
below. The new pass verifies this selected population, **not maximum capacity,
500,000 users, or an organizer score**. Host variation means the precise
before/after latency ratio is not an isolated causal measurement. It is also
not a matched comparison against the Rust submission on the organizer's VM.

See [research/mjc-275k-fifo-five-minute.json](research/mjc-275k-fifo-five-minute.json)
for the exact binary/generator hashes, limits and native results.

## Receive-retry fairness investigation

The receive-retry candidate is
`4c195011c6776e03a70e1808bec4ad6a90840535aa8b82fecec86a5c710e96d3`.
An instrumented repeat of the preceding 275,000-user five-minute workload
failed again: p95 71.853 ms, p99 1,304.078 ms, zero HTTP/check failures. The
long requests now clustered around the end of ramp-up/early hold, not the same
two windows as the earlier failure. One-second read-only snapshots captured
**32,375 deferred receives in one ring and 56,568 across all rings**. This
identified a server-side mechanism that can amplify a temporary traffic burst
or scheduling stall, without establishing the trigger of every original stall.

The old retry policy required the completion queue to become entirely empty.
Continuous completions could therefore delay older ENOBUFS-paused connections
for seconds. The candidate makes two changes:

- A nonempty completion queue can still make bounded retry progress, paced by
  buffers returned during that owner visit, capped at 128 retry entries.
- While a retry FIFO exists, fresh receive rearms join its tail instead of
  bypassing older paused connections. The drain submits the FIFO head directly.

Credits control retry work; they do not claim exclusive ownership of buffers
that another pending kernel receive might consume. Generation/closing checks
are retained. Buffer sizes, ring count, SQLite settings and resource limits
are unchanged. `--enobuf-drain=empty` selects the previous policy for controls.

### Controlled overload screens (not qualification)

Each trial used 275,000 users, a 60-second ramp, one-minute hold and normal
ramp-down, without warmup. At approximately 80 seconds after launching the
generator, the server cgroup was paused for half a second and then resumed.
Read-only queue/scheduler sampling was enabled. The freeze deliberately
accumulates requests beyond the provided-buffer capacity.

| Policy | p99 | Requests over 1 s | First sampled empty retry queues after resume |
| --- | ---: | ---: | ---: |
| Original empty-CQ gate | 118.716 ms | 13,260 | 3.569 s |
| Returned-buffer credits only | 154.725 ms | 7,018 | 3.181 s |
| Credits plus FIFO admission | 89.482 ms | 3,196 | 2.127 s |

All three screens had zero HTTP/check failures, clean shutdown and successful
database integrity checks. The credits-only change did not improve aggregate
p99; FIFO admission was needed for the final candidate. One-second sampling
only bounds observed recovery, and sequential shared-host trials do not isolate
all variation. The new FIFO also contains fresh rearms, so its raw queue length
is not directly equivalent to the old count of ENOBUFS failures. The separate
over-one-second request counts matter more than a short-run p99 alone.

Official checks passed 42/42 on the FIFO candidate. Deterministic tests execute
the real NASM drain function against fake ring state: old-policy starvation,
credit bounds, FIFO ordering, generation safety, closing/pending receives and
logical ring addressing. One-shot and multishot integration/crash recovery,
5,000-connection feed bursts and fragmented-upload pressure tests also passed.
After confirmation, forced-epoll integration/crash recovery and the 80,000-live-
socket teardown regression passed too (fresh health response in 0.969 seconds
after starting disconnects).
`tests/enobuf-policy.py` needs GCC, binutils and system SQLite only to resolve
unused symbols in its test executable; the submitted server still uses the
pinned source-built SQLite. The C test driver is never linked into that server.

Exact hashes, configurations, native reports, slow-request counts and selected
queue observations are in [research/enobuf-tail-investigation.json](research/enobuf-tail-investigation.json).

## Before fairness: native 275,000-user five-minute failure

The preceding teardown-fixed binary was
`3a07352ab36159baf611f82fcaaa13d91939f415e05a70c7f7a3d7a0dfbe35d4`.
It had **not passed a five-minute test above 250,000 users**. The native
`mjc/twelve-dollar-benchmark` trial reached 275,000 users and completed a
300.001-second full-load hold, but failed the p99 threshold:

| Users | Requests | p95 | p99 | HTTP/check failures | Peak server cgroup |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 275,000 | 10,173,661 | 74.842 ms | 3,543.983 ms | 0 / 0 | 1,539.0 MiB |

Latency percentiles cover the complete native run, including ramp and drain;
the hold itself averaged 28,765 requests/s. Warmup was 1,000 users for two
minutes, then the main trial used a 60-second ramp, five-minute hold and normal
30-second ramp-down/10-second grace. There were no OOM events or TCP
retransmissions, and the server shut down cleanly. Failure is failure: keeping
the users connected does not qualify without passing latency thresholds.

The generator is pinned to `e31cd6285704b436422c2fc100d3beb10e558f6c`, built
with Rust 1.99.1 and locked dependencies on Ubuntu 24.04; its 82 tests passed.
Server limits are one CPU, 2 GiB, no swap and nofile=65,535. The generator has a
separate 12-GiB cgroup, nofile=1,048,576, four Tokio workers, and 64 loopback
source IPs. CPUs 0 and its SMT sibling 8 are excluded from client placement.
Private same-host loopback, host resources outside the server cgroup and newer
hardware/kernel make this **not an organizer score**. Native HTTP latency
excludes DNS/TCP setup; connection timing is reported separately.

The generator observed 275,000 established connections; the shared namespace
has two TCP endpoints per connection, not twice as many users. The native
server descriptor census cannot see registered io_uring sockets. See the
[selected native evidence](research/mjc-275k-five-minute-failed.json).

An earlier attempt exposed a mass-disconnect failure after the five-minute
hold. Closing every socket used FD-wide cancellation even when no receive or
send remained pending. Teardown now cancels only outstanding operations by
their exact generation-tagged completion key. A 275,000-user/15-second-hold
screen passed after this change (p95 35.005 ms, p99 92.412 ms, zero errors), and
the full failed-latency run above also completed teardown. The functional
80,000-live-socket regression verified a new health response 1.071 seconds
after starting to close all clients. Official checks passed 42/42 and both
one-shot and multishot io_uring integration/crash recovery passed. These
checks establish correctness of teardown, not the missing capacity score.

A subsequent two-minute diagnostic hold at 275,000 users, without warmup,
passed with p95 36.630 ms, p99 83.685 ms and zero errors. A read-only sidecar
sampled ring state once per second and found no persistent deferred-buffer
backlog. The five-minute tail failure was not reproduced and its cause remains
unresolved. This shorter run does **not** supersede the failed five-minute
qualification. Its [selected evidence](research/mjc-275k-two-minute-diagnostic.json)
records the additional instrumentation and different duration.

## 2026-10-23 mixed profiling and checkpoint audit

The preceding checkpoint-guard binary was
`6736582bea97ca87235ba66fbf69ef8befe172eeb1f8bf9318e9258da17c91f3`.
The measurements below use an AMD Ryzen 7 PRO 8845HS, Linux 6.18.18, GCC 15.2.0
through Nix, and source-built SQLite 3.54.0. These are local diagnostics, not
organizer scores. Earlier experiments are retained separately below.

### Final unchanged-k6 five-minute confirmation

That exact binary passed 40,000 users after a 1,000-user/two-minute
warmup, 60-second ramp, **five-minute hold**, and normal 30-second ramp-down:

| Users | Requests | p95 | p99 | HTTP/check failures | Peak server cgroup |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 40,000 | 1,493,768 | 0.668 ms | 4.972 ms | 0 / 0 | 580.2 MiB |

Server limits: one CPU, 2 GiB, no swap, nofile=65,535. k6 1.6.1 ran in a
separate 18-GiB/no-swap container, on CPUs 1-15; server threads shared CPU 0.
They used a private loopback network with 32 source IPs. The host OS and other
services are outside the server cgroup. Docker's seccomp filter was disabled
for io_uring. No host sysctls were changed. These differences from the droplet
mean this is **not an organizer score or a maximum-user search**.

There were no OOM kills, shutdown exited successfully, and `integrity_check`
returned `ok`. The WAL repeatedly reset rather than growing through the hold.
Peak cgroup memory was approximately 71% lower than the preceding full run.
The lower p95/p99 in this run should not be treated as an isolated causal
latency improvement: these are two sequential trials on a shared host.

Official checks passed 42/42; primitive tests, io_uring and epoll integration/
crash recovery, and all 18 range/transaction differential fixtures passed.
Reproducible timings, binary/workload hashes, resource samples and final states:
[bounded run](research/bounded-40k-five-minute.json) and
[preceding passive-only run](research/passive-40k-five-minute.json).

500,000 users remain a target, not a demonstrated result. The native maximum
search is still pending; the newer fixed-population trial is described above.

### Why the restart guard was added

The first full 40,000-user run passed the unchanged k6 workload after the
required warmup, with a 60-second ramp and five-minute hold: 1,493,970 requests,
p95 0.780 ms, p99 9.011 ms, and zero HTTP/check failures. However, server-cgroup
memory peaked at 2,001.8 MiB and the WAL reached about 1,426.5 MiB. That run used
the preceding `144c8277406ab683cfad5e26607efe283b7e06477eee627bd0f9c0dd6cbbbfab`
binary. It is not evidence for the final checkpoint policy.

Inspection during sustained traffic showed that the helper had copied all
245,848 frames in its checkpoint snapshot, while the writer had already reached
248,469. Passive copying was progressing, but continuous writes prevented WAL
reset. The owner now attempts `SQLITE_CHECKPOINT_RESTART` at a transaction-free
batch boundary once 32,768 frames are reached. It does not wait on a competing
checkpoint lock; `SQLITE_BUSY` is retried at the next boundary. This is a restart
guard, not an unconditional size cap in the presence of external readers.

### Mixed saturation comparison

`tools/profile.py` uses `wrk` 4.2.0, 128 persistent connections and four client
threads. The Lua workload samples endpoint proportions 100:100:15:2 and checks
response statuses/feed cardinality, but omits think time. Each trial starts
from a fresh seed, warms for five seconds and measures for 25 seconds. Server
limits are one CPU, 2 GiB, no swap and nofile=65,535; the client is in a separate
cgroup. CPU placement and same-host private loopback differ from the organizer.

The same binary was run in A/B/B/A order, selecting the old policy with
`--checkpoint=passive`:

| Trial | Policy | req/s | p95 ms | p99 ms | Peak cgroup MiB |
| --- | --- | ---: | ---: | ---: | ---: |
| A1 | Passive only | 70,706 | 2.698 | 5.134 | 1,535.9 |
| B1 | Restart guard | 74,350 | 3.100 | 27.057 | 408.5 |
| B2 | Restart guard | 65,206 | 2.958 | 19.584 | 405.7 |
| A2 | Passive only | 78,866 | 2.436 | 13.012 | 1,636.2 |

All trials had zero invalid responses and no reported transport errors. Memory
includes charged page cache, not just process RSS. The guard reduces memory
pressure, but is **not a demonstrated throughput win**: average throughput was
about 6.7% lower, and short-run p99 increased. Host variation is substantial.
The guard is retained for sustained-memory headroom, not advertised as faster.

A second C/D/D/C experiment tested `--batch=128 --wait-us=0` against the default
32/250-us policy for 20 seconds per trial. Defaults produced 69,360 and 68,611
req/s; the candidate produced 72,183 and 66,381. The direction reversed between
pairs, so the change was rejected. No defaults were changed on that evidence.

A separate 25-second sampled profile collected 4,474 cycle samples with none
lost. The largest named symbols were SQLite VDBE execution (10.81%), B-tree
table lookup (8.05%) and WAL-frame lookup (7.38%). SHA compression rounds were
0.65%; this does not motivate prioritizing a SHA-NI rewrite. These are flat
samples, not inclusive call-tree costs. Kernel samples were collected but their
symbols could not be resolved under the host's address restrictions. Profiled
throughput (69,025 req/s) is diagnostic and excluded from the paired comparison.

Raw comparisons and the symbol report are in
[research/mixed-profile-evidence.json](research/mixed-profile-evidence.json)
and [research/mixed-profile.txt](research/mixed-profile.txt).

## Earlier experiments (historical binaries)

This is a candidate comparison, not an organizer score. The inherited assembly
baseline has separately reported a 275,000-user five-minute pass; reproducing a
population that large requires a load generator with roughly 85 GiB of memory,
which was not available in this workspace.

The release binary was tested on 2026-10-23 with the server pinned to CPU 0 and
four `wrk` 4.2.0 threads pinned to CPUs 1-4. Each trial used 128 persistent
connections for 10 seconds against `GET /feed` and the canonical seed. Runs were
ordered baseline, optimized, optimized, baseline to expose thermal/time drift.

The baseline controls were selected in the same binary with:

```text
--collect=baseline --quote=scalar --literal-copy=scalar --integer=div
```

| Pair | Baseline req/s | Optimized req/s | Gain |
| --- | ---: | ---: | ---: |
| 1 | 47,142.87 | 50,152.38 | 6.38% |
| 2 | 41,975.15 | 44,815.75 | 6.77% |

Across the four short trials p99 ranged from 3.90 to 5.29 ms. There were no
transport or HTTP errors. This saturation workload removes the challenge's
think time and tests hot-path CPU throughput; it does not establish a maximum
VU score or isolate each optimization. A matched five-minute run above 275,000
users remains the required proof of a leaderboard win.

## Integer hot path

The next candidate bypasses the general formatter for the overwhelmingly common
one- and two-digit like counts. Two 15-second opposite-order comparisons kept
all other optimized defaults fixed:

| Pair | Division req/s | Fast formatter req/s | Gain |
| --- | ---: | ---: | ---: |
| 1 | 49,642.86 | 56,505.90 | 13.82% |
| 2 | 48,766.42 | 50,405.59 | 3.36% |

A follow-up decimal-pair formatter for larger post IDs averaged about 5.2% below
the simpler reciprocal-multiply loop and was removed.

## Challenge workload

The exact `bench/load.js` workload was run with a 15-second ramp, 30-second hold
and normal 30-second ramp-down. The server was pinned to one CPU, k6 used the
other local CPUs, and the 30k+ runs used four loopback source IPs to avoid the
load generator's ephemeral-port ceiling.

| Users | p95 | p99 | Failed | Requests | Result |
| ---: | ---: | ---: | ---: | ---: | --- |
| 20,000 | 1.17 ms | 14.55 ms | 0 | 127,804 | pass |
| 30,000 | 1.52 ms | 36.20 ms | 0 | 191,705 | pass |
| 35,000 | 15.33 ms | 160.13 ms | 0 | 223,606 | pass |
| 40,000 | 699.17 ms | 957.45 ms | 0 | 250,865 | fail p95 |

Thus 35,000 is the highest locally verified passing population and the observed
short-run boundary is between 35k and 40k. The 40k p95 jump occurred on a shared
host running unrelated high-CPU services; these short same-host results are not
a substitute for the challenge's separate-generator five-minute confirmation.

Correctness validation for this exact source:

- Official challenge suite: 42/42.
- Primitive suite: SHA/HMAC/base64url, NumericDate, 2,000 randomized JSON cases,
  ABI checks, canaries, and guard-page tails.
- Integration suite on raw `io_uring` and forced `epoll`: auth, Unicode, foreign
  key rollback, duplicate likes, fragmented and chunked bodies, `100 Continue`,
  framing rejection, commit durability, crash recovery, and integrity check.

The locally built release binary SHA-256 was
`17ef302a0063a4338480f2fb8a3948895d7a28bfe81419525c5c89d580ec00f7`.
The build ran under Nix and suppressed `-march=native`; a clean Ubuntu 24.04
build will retain the intended native-code flag and therefore have a different
binary identity.

## 500k-oriented candidate

The next candidate moves passive checkpoint copying to a dedicated SQLite
connection and pthread. SQLite is compiled from the pinned 3.54.0 amalgamation
with `SQLITE_THREADSAFE=2`; both connections use `SQLITE_OPEN_NOMUTEX` and the
stock `unix-excl` VFS. Checkpoints are requested every 4,096 additional WAL
frames, and `journal_size_limit=16777216` limits retained WAL after reset. This
removes the request owner's previous synchronous checkpoint every 1,000 frames.

The feed path now combines newest-row discovery and row serialization, followed
by one bounded like-ID scan. It no longer reads the same 20 post rows a second
time. Sparse spans and ranges with more than 4,096 likes retain the general live
query fallback. All results remain request-time SQLite reads.

At 128 persistent feed connections, the old default produced 48,310 and 50,831
req/s in two trials. Enabling the three-scan path produced 56,525 and 59,673
req/s. After removing the redundant row query, three trials produced 62,921,
61,038 and 54,041 req/s. Host variation is substantial, but the direction is
consistent and the final median is about 23% above the initial two-trial median.
These sequential trials did not isolate host drift, and do not establish a
causal 23% improvement in the mixed workload.

Before the two-query change, the challenge workload was run at 40,000 users with a 15-second
ramp, 30-second hold and normal ramp-down. The server was pinned to one CPU and
k6 to the other local CPUs with four loopback source addresses. It completed
255,685 requests with zero HTTP or semantic failures, p95 1.32 ms and p99
56.09 ms. The preceding build on the same shared host had failed this point at
p95 699.17 ms and p99 957.45 ms. This short shared-host comparison is not an
organizer-equivalent capacity score.

Eight 65,535-slot direct-descriptor tables provide 524,280 physical slots. A
functional isolated-namespace probe opened and retained 450,000 client/server
pairs before same-kernel connection establishment became impractically slow.
At that point `/proc/net/sockstat` reported more than 913,000 TCP endpoints.
The server remained alive, but the probe never reached its response-validation
phase, so this is neither a verified live-socket count nor a 500k pass. The
initiating bottleneck was not established. A later audit found adaptive mode
incorrectly started every ring immediately; that startup bug is now corrected.
Adaptive acceptance is intended to preserve the low-load cascade and activate
the last four rings together near 260k sockets; high-population benefits remain
unverified.

Final validation for this source:

- Official challenge suite: 42/42.
- Extended auth, Unicode, framing, transaction rollback, duplicate-like,
  commit durability and SIGKILL recovery suite: pass.
- Dense, sparse, interleaved, skewed and signed-ID feed differential matrix in
  per-request, writes-only and batch transaction modes: pass.

The local release binary SHA-256 before the acceptance/shutdown audit was
`11bbb55a9ee1b927b4768cd3208def9a980f151f3eee776c66f46f065fa6c408`.
The next required evidence is a separate-generator, 2-GiB/no-swap five-minute
search at 400k, 450k and 500k; nothing here claims that result in advance.
