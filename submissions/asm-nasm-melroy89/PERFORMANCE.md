# Performance and qualification evidence

Work in progress. No organizer score, maximum-user search or win over the eligible
field is established.

## Verified locally

- Official 42/42 checks passed on epoll and raw io_uring.
- A 27-case raw io_uring matrix covering reference/selected/range SQL,
  SQL/NASM/hybrid serialization and per-request/writes/batch transactions passed
  42/42 in each configuration. Each also matched a reference SQLite fixture with
  sparse IDs, reversed/tied timestamps, Unicode and skewed likes.
- Test-only SQLite boundary injection passed failed COMMIT and transaction-wide
  rollback cases in all three transaction modes. Successful replies survived
  restart, and successful feed reads exposed no subsequently rolled-back rows.
- Integration checks passed for foreign-key failures followed by valid writes,
  concurrent duplicate likes, fragmented Unicode, chunked bodies/100-continue,
  framing ambiguity and acknowledged-write SIGKILL recovery.
- Independent primitive tests passed 2,000 randomized JSON cases, SHA/HMAC,
  NumericDate/base64url checks, guard-page tails, canaries and ABI preservation.
- Aggregate input-pressure testing passed: admitted split-UTF-8 uploads completed
  with response capacity reserved, and admission recovered.
- The source-built Ubuntu 24.04 image completed root installation and normal-user
  PGO build. This verifies userspace packaging, not an Ubuntu 6.8 kernel.
- Native functional proof: 80,000 simultaneously open sockets each returned a
  valid health response with server RLIMIT_NOFILE soft/hard 65,535.
  Host kernel 7.0.0-38-generic; elapsed 3.36 seconds; process RSS about 16.5 MiB.
  This is not a constrained k6 capacity score or total socket memory measurement.

Generated raw identities/results live in ignored results/ and build/ directories.
Re-run checks after changes; the recorded binary identity bounds each result.

## Initial unchanged-k6 diagnostic

Image binary SHA-256:
f8674aba5a85cacb386865fcc8fa18fb89684650d4ee010e0aa9bbac205ebf1a

10,000 users, 10-second ramp, one-minute hold, no prescribed warmup:
92,917 requests, zero HTTP failures, zero failed semantic checks,
p95 0.207 ms, p99 0.347 ms, peak server cgroup memory 315.7 MiB.

Server container: one CPU quota, 2 GiB, no swap, nofile=65535.
k6 1.4.2 ran outside that quota on the same host with the unchanged bench/load.js.
This short run is diagnostic only. Subsequent source changes are not covered
by this binary's performance measurements.

## Selected 100,000-user five-minute confirmation

The latest source-built Ubuntu image passed unchanged k6 1.4.2 at 100,000 users
after a two-minute 1,000-user warmup, a 60-second ramp, a five-minute hold and the
normal ramp-down:

| Metric                                      |    Result |
| ------------------------------------------- | --------: |
| Requests (full scored run, including ramps) | 3,736,531 |
| Failed HTTP requests                        |         0 |
| Failed semantic checks                      |         0 |
| p95                                         |  0.719 ms |
| p99                                         |  6.025 ms |
| Peak server cgroup memory                   | 711.6 MiB |
| OOM killed                                  |        No |

Host: AMD Ryzen Threadripper 7960X (24 cores/48 threads).
Server: one CPU quota, 2 GiB, no swap, nofile=65535, Ubuntu 24.04 userspace on
Linux 7.0.0-38-generic. Generator ran on the same host outside the server quota,
with multiple loopback source IPs. Percentiles are the unchanged script's
full-run aggregates, including ramps, not separately calculated hold-only values.

Measured binary SHA-256:
f716e6b52e18ad8e444625a9278d812f72cbef734213b372489f476532426f87

This is a selected passing local load, not a maximum search, separate-machine
DigitalOcean result or proof of superiority over the eligible field. Source
hashes and image/build identities are in results/local-100000-evidence.json.

## Reproduction

From this submission directory:

```bash
docker build -t twelve-nasm-qualification:local .
python3 benchmark.py --vus 100000 --label local-100000
```

The harness uses the unchanged workload, checks semantic failures explicitly,
records image/build/workload identities and reads cgroup peak memory/OOM status.
Default warmup is two minutes at 1,000 users and default hold is five minutes.
Full-run percentiles include ramps, as the official script does.

Local Docker needs seccomp=unconfined for io_uring; this is a container-policy
exception, not host kernel tuning. The organizer's native environment is separate.
The host OS is outside the server container budget. A separate-machine droplet
run remains necessary for an official comparison.

## CPU saturation screens (substitute generator)

These are same-host diagnostics with four autocannon 8.0.0 processes, 16 active
connections each, no think time, fresh seed per trial and the same one-CPU/2-GiB
server limits. A 217-request cycle contains 100 feed, 100 post, 15 like and two
create requests, choosing post IDs from live feeds. Response callbacks check
status and response shape/IDs. This generator is not unchanged bench/load.js,
does not establish user capacity, and cannot rank unlike external results.
The initial generator starts the four processes at the same token index; later
screens use distinct offsets. There is no warm-up in these short screens.

All eleven initial five-second trials and six alternating ten-second JSON trials
had zero transport, timeout, non-2xx or semantic errors. Rates below use actual
reported client durations, which can exceed the requested duration.

| Initial screen                              | Requests/s (approximately) |
| ------------------------------------------- | -------------------------: |
| Reference SQL + SQL JSON                    |     47,749 / repeat 50,652 |
| Selected-ID aggregation                     |                         60 |
| Bounded range aggregation                   |                     14,871 |
| Reference SQL + NASM JSON                   |                     51,781 |
| Hybrid JSON                                 |                     47,034 |
| Shared transaction, 32 completions / 250 us |                     50,900 |
| Shared transaction, 128 completions / 6 ms  |                      4,107 |

The 128-completion target exceeds the 64 active clients in this test, so bounded
waiting dominates. It is not evidence that this setting behaves identically with
larger active populations. Selected-ID aggregation needs query-plan inspection;
its semantics passed, but this implementation is unsuitable as a default.
Per-request/writes/shared-transaction five-second screens were approximately
48,723/48,894/50,006 requests/s; this difference alone does not select a winner.

Alternating SQL/NASM/NASM/SQL/SQL/NASM trials averaged 50,701 versus 53,393
requests/s; requests per server CPU second averaged 50,875 versus 54,365
(about 6.9% improvement). Each adjacent pair favoured NASM, but host noise and
short trial duration remain limitations. Release defaults remain unchanged
pending further qualification, including large idle populations.

Reproduce from repository root (requires locally installed autocannon 8.0.0 at
/usr/lib/node_modules/autocannon and Node.js; generator dependencies are not
server dependencies):

```bash
python3 submissions/asm-nasm-melroy89/tools/screen.py --duration 10 --label json-screen --variant sql: --variant nasm:--json=nasm
```

Raw files: results/initial-transaction-screen.json,
results/initial-query-transport-screen.json and results/paired-json-screen.json.
All measured the same packaged binary f716e6b52e18ad8e444625a9278d812f72cbef734213b372489f476532426f87.

## 150,000-user short screen

Unchanged bench/load.js, fresh seed, same packaged binary and server limits:
30-second ramp, 30-second hold, 30-second ramp-down, no warmup. The run completed
1,078,049 requests with zero HTTP failures and zero status-check failures.
Full-run p95 was 1.524 ms, p99 7.758 ms, maximum 43.303 ms; peak server cgroup
memory was 876.9 MiB. Raw evidence: results/screen-150000-evidence.json.
This short screen is not five-minute qualification or a maximum-user score.

## 150,000-user five-minute confirmation

The unchanged workload passed after the standard 1,000-user/two-minute warmup,
60-second ramp and five-minute hold. Same Threadripper host, Ubuntu userspace,
Linux 7.0 kernel, one server CPU, 2 GiB with no server swap, inherited nofile 65,535,
host loopback networking and separate k6 container outside the server budget.

- 5,603,813 requests; zero HTTP failures and zero status-check failures.
- Full-run p95 1.900 ms, p99 7.444 ms, maximum 41.197 ms.
- Peak server cgroup memory 966,434,816 bytes (921.7 MiB); no server OOM.
- Server CPU 197.64 seconds over 570.17 seconds including warmup and ramps.
- Same measured binary f716e6b52e18ad8e444625a9278d812f72cbef734213b372489f476532426f87.
- Raw results/identities: results/local-150000-evidence.json and companion logs.

This is a selected-count local confirmation, not the prescribed maximum search,
an organizer score or a win over the field. The generator approached its 48-GiB
container allowance. Later private-namespace tests must be identified separately.
The subsequent small-limit ring fix is not included in this measured binary.

## Query-only SQLite profiling and ring-limit regression

The NASM diagnostic profiler links the exact measured image's SQLite release
object (SHA-256 44e0a3b1ba9200da9a3f7c7dffe7f40f90600287cb52bf2cc7ff9694dcc35d17).
It enables SQLite memory counters and wraps its original allocator in assembly,
counts API calls, copies each returned column before reset and uses process CPU
time. It opens the seed read-only with 1-GiB mmap/500-page cache. Ten iterations
include first-query cold effects; these are query diagnostics, not full HTTP or
NASM serialization timings. Memory highwater includes connection/statement state;
allocation calls count query execution malloc/realloc separately, not live blocks.
Counter definitions were checked against the pinned sqlite3.h documentation.

| Query                          | CPU us/feed | VM steps/feed | Full-scan steps/feed | Allocation calls/feed |
| ------------------------------ | ----------: | ------------: | -------------------: | --------------------: |
| Reference SQL JSON             |       29.25 |           827 |                   19 |                  24.1 |
| Reference plain rows           |       26.43 |           709 |                   19 |                   1.1 |
| Selected-ID SQL JSON           |    41650.42 |      10083211 |              2016042 |                  63.1 |
| Range SQL JSON                 |       40.59 |          2541 |                   57 |                  61.1 |
| Forced selected-first SQL JSON |       41.91 |          2425 |                   57 |                 141.1 |

A diagnostic CROSS JOIN forces selected posts before likes, removing the massive
scan, but extra materialization/grouping still loses to the reference on this
seed. It is a tested profiler variant, not an adopted server query. More skewed
and evolving datasets remain relevant. Raw output: results/query-profile-measured.json.

A new native functional test exposed unsigned underflow in ring activation when
the inherited descriptor limit was below 256. The source now scales the reserve
to min(256, slots/4). The diagnostic build passed 200 simultaneous sockets with
nofile=128 and 300 with nofile=64; all returned valid health responses while the
other connections remained open. This does not replace the actual Ubuntu-kernel
or constrained capacity gates. Logs: results/low-nofile-before.log,
results/low-nofile-after.log and results/low-nofile-64.log.

## 250,000-user short screen: failed

Private shared loopback namespace, unchanged bench/load.js, same measured release
binary, fresh seed, 30-second ramp/30-second hold, no warmup, and an 80-GiB
same-host generator allowance. Server limits remained one CPU, 2 GiB, no swap,
nofile=65,535. The namespace avoided the host conntrack limit without host tuning.

The run failed: 1,021,794 requests, 329,152 HTTP/semantic failures (32.21%),
p95 37.22 seconds, p99 55.92 seconds and peak server cgroup memory 1,773.4 MiB.
No server OOM was recorded. Server CPU samples reached its quota; generator CPU
was also briefly very high during ramp-up. Client logs contain timeouts and
connection resets. These observations do not yet identify a unique root cause.
Receive admission, provided-buffer recovery, completion scheduling and generator
bursts require instrumentation/controlled experiments before retesting. Do not
count this as capacity, extrapolate a maximum, or attribute all failure to SQLite.
Raw evidence: results/screen-250000-private-evidence.json and companion log.

The small-limit ring correction separately passed official 42/42 and integration
checks on its diagnostic binary, recorded in results/ring-margin-evidence.json.
It was not included in either the 150k confirmation or failed 250k screen.

## Receive-pressure diagnosis and 250k candidate screen

A release/PGO build with compile-time I/O counters reproduced the 250k failure
with the same 30-second ramp/hold and private namespace. It failed with 37.66%
HTTP failures and p99 56.89 seconds. Final sampled counters showed 7,753,700
ENOBUFS completions, 184,941 input-budget failures, 112,039 output-budget
failures and CQ depth reaching 8,192. Immediate rearming was amplifying exhausted
provided-buffer work rather than allowing replies/resources to drain. Raw:
results/screen-250000-io-diagnostics-evidence.json; layout in io-counter-layout.json.
Counters compile out of ordinary builds.

Two experimental controls were implemented, leaving defaults unchanged:

- send-first: handle up to 64 send/close completions from a frozen CQ boundary
  before FIFO receives. Handled CQ entries remain owned until head publication
  and are marked to prevent duplicate processing. Receive work retains at least
  half the normal completion-count budget; time checks remain bounded.
- enobuf=defer: park generation-tagged retry IDs in a bounded per-ring FIFO.
  Rearm at most 128 entries after draining the frozen CQ, outside transactions.
  Stale generations are discarded, paused receives cannot be armed twice, and
  queue-full retirement allows an already reserved response to drain. Retry
  queues add four MiB of lazily touched storage, beyond the 64-byte records.

Native diagnostic validation passed six integration combinations (all three
transaction modes, both receive modes), the 27-case official/query differential
matrix, input pressure and injected commit/automatic-rollback failures. A
SIGSTOP-induced burst passed all 5,000 exact-reference feed responses in 0.155 s;
this is a functional test, not a constrained capacity score. Raw logs use the
results/receive-controls-* prefix; native binary was
fbee39bf57cdd6cd66cbffcf36aeef4f85f121e21acd2b2aa90926f9a310130c.

With just send-first/deferred recovery and the original 30-second ramp, the 250k
screen had zero failures over 1,779,149 requests, but p99 1.586 s still failed.
Peak memory was 1,294.5 MiB. This removes request loss but does not pass latency.
Raw: results/screen-250000-receive-controls-evidence.json.

The next combined candidate used shared frozen-batch transactions, NASM JSON,
32 completions/250 us, both recovery controls, and the official 60-second ramp.
Its 30-second hold passed: 2,193,302 requests, zero HTTP/semantic failures,
p95 9.717 ms, p99 15.459 ms, maximum 50.291 ms, peak memory 1,313.9 MiB.
The generator sampled peak was 69.70 GiB of its 80-GiB limit, with no generator OOM.
This is a combined-configuration screen; ramp and multiple settings changed,
so it does not isolate the contribution of each optimisation. It is not a
five-minute score, a maximum search or a whole-field win.

Measured release/PGO diagnostic binary:
48c67f4e7518d2560d82742d24b5ca64fd1d521ba63cabebfbd0be22ddebf7d1.
Image: sha256:705a9828c85bfae0b32b4351b2422ab1d987bd8572917c8b0996a03620c1fd9a.
Raw: results/screen-250000-batched-nasm-evidence.json and its generator resource
record. The benchmark harness now samples generator cgroup CPU/memory and
retains exit/OOM state before removing its container, making generator failures
separate from server threshold failures.

The full confirmation subsequently passed with the same image/configuration,
standard 1,000-user/two-minute warmup, 60-second ramp and five-minute hold:

- 9,334,974 requests; zero HTTP failures and zero status-check failures.
- Full-run p95 9.004 ms, p99 13.299 ms, maximum 84.445 ms.
- Peak server cgroup memory 1,354,076,160 bytes (1,291.3 MiB); no server OOM.
- Generator sampled peak 76.99 GiB of its 80-GiB limit; no generator OOM.
- Raw: results/local-250000-batched-nasm-evidence.json, summary, log and generator records.

The server had one CPU, 2 GiB and no swap; kernel 7.0.0-38-generic, Ubuntu 24.04
userspace, private shared loopback, same-host k6 outside the server quota. This
is a selected-count local confirmation, not an organizer score, prescribed
maximum search or whole-field ranking. Per user direction, further work stays
in Docker for now; actual Ubuntu-kernel qualification is deferred.

The verified combined configuration is being promoted to candidate defaults,
with the I/O instrumentation compiled out. Its clean Ubuntu Docker build completed
root installation and normal-user PGO compilation. Unchanged start.sh runs the
server as PID 1, non-root, using only the four server configuration variables.
Both raw io_uring and epoll fallback passed 42/42; health was reached in 0.237 and
0.203 seconds. Evidence: results/candidate-package-evidence.json. Selected-count
performance confirmation on this uninstrumented binary remains separate.

## Canonical seed verification

The current seed's complete ordered-row content checksum matches seed/make-seed.sh:
738d11ab3164c13177e347f75038d1e80b43c64e754544a42d1c32bd8d7bcf1d.
It contains 50,000 users, 500,000 posts and 2,016,005 likes; the upstream token
file intentionally contains 20,000 tokens. Verification opened the seed read-only.
Raw: results/seed-content.sha256. All runs start from a fresh seed copy.

## First matched pinned-contender screens

Sequential fresh-seed 250k screens used unchanged bench/load.js, the official
60-second ramp, 30-second hold, no warmup, private loopback, k6 1.4.2 with an
80-GiB generator allowance, and identical one-CPU/2-GiB/no-swap/nofile=65,535
server limits. Both start.sh scripts ran unchanged. Both passed all HTTP and
semantic checks. This pair is not a maximum search or five-minute comparison.

| Candidate | Requests | Full-run p99 | Peak server MiB | Server CPU us/request |
| --- | ---: | ---: | ---: | ---: |
| Uninstrumented NASM defaults | 2,194,058 | 13.290 ms | 1488.1 | 30.214 |
| Pinned Rust #19 | 2,194,599 | 7.035 ms | 1248.7 | 27.562 |

NASM user/system CPU was 13.038/17.177 us per request; Rust was 9.798/17.764.
These include ramp/accept/close work and are not pure handler timings. One pair
has limited variance evidence. Rust remains competitive; lower latency here does
not establish its higher maximum, and NASM has no win claim. Kernel/slab snapshots
after generator exit also depend on how quickly outstanding socket closes drain.
Raw: results/matched-250000-nasm-default-evidence.json and
results/matched-250000-rust19-evidence.json, with generator resource records.
NASM binary: 88cedc5688781ec6550ec4df062a6cb75b24be67024a19e635c4ac9126c5e011.
Rust binary: d5a17ae238bcbf5d2c07bbf392d2a1795fec31b3e56f9b600bc99ae8662522b8.

## Optional SSE2 JSON quote experiment

An opt-in --quote=vector path scans/copies 16 ordinary bytes at a time using
baseline x86-64 SSE2, finds the first quote/backslash/control byte with a mask,
and retains scalar escape/tail handling. It never loads a full vector with fewer
than 16 source bytes remaining. --quote=scalar remains the default/control.

Validation passed randomized JSON, input and output guard pages, canaries and
callee-saved ABI checks, then server integration and all 27 official/query
matrix configurations. Generated logs: results/vector-integration.log and
results/vector-matrix.log; primitive tests now exercise vector boundaries too.

Six alternating ten-second saturation trials used the same release/PGO binary
and four autocannon clients processes, with all other candidate defaults fixed.
All transport/status/semantic validations passed. Aggregate requests per server
CPU second were approximately 55,917 scalar versus 56,689 vector (about 1.4%).
Two adjacent pairs favoured vector; one regressed. The result is small and noisy,
so vector is retained as an experiment, not adopted or claimed as a capacity gain.
Raw: results/paired-vector-screen.json. Image:
sha256:0329c42468bd4fe7182d4bbb33dccadf85bf4176237ba3568e47bceb13a060e9.
The earlier 250k confirmation and matched contender pair predate this experiment.

## Bounded additional optimization session

See [research/optimization-window.md](research/optimization-window.md) for
experiments, semantics, raw-result locations and failed controls. Three paired
short CPU trials favoured disabling redundant write savepoints by about 2.9%;
that change is now the default. Ordinary constraint failures preserve other
writes through SQLite statement rollback, while unexpected/transaction-wide
failures invalidate the batch. Mixed valid/invalid write/read bursts and injected
failed COMMIT/automatic rollback passed.

The bounded range-query path was about 5.7% slower and remains disabled.
Multishot receive improved one saturation screen but failed a 275k short
population screen (p99 1,187.6 ms). The one-shot control passed at 275k with
p99 90.9 ms and zero HTTP/check failures. Neither short run is a five-minute score.

Current image twelve-nasm-optimized:local:
sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16.
Both unchanged-start io_uring and explicit epoll fallback passed 42/42 official
checks, foreground normal-user startup, no diagnostic counters and healthy
within 0.25 seconds. Raw: results/optimized-package-evidence.json.
Final 275k confirmation: PASSED. Unchanged bench/load.js, 1,000-user
two-minute warmup, 60-second ramp, five-minute 275,000-user hold and standard
ramp-down. 10,264,833 measured requests, zero HTTP failures and zero check
failures; p95 16.233 ms, p99 65.394 ms, max 2,436.599 ms. Peak server memory
1446.0 MiB/2 GiB; no OOM. The current unchanged
start.sh and default settings were used.
Generator peak 85.14 GiB/92 GiB,
no generator OOM. Percentiles cover the measured ramp/hold/ramp-down together.
This is a selected-count local confirmation, not a maximum search or organizer
score. Exact runtime binary: e511302626f6f0e06c2981aeb19e4d90923afeebf497ff711373d791f9a8491e.
Exact image: sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16.
Raw: results/optimized-275000-confirmation-evidence.json, its k6 summary/log,
and generator resource record.

## Remaining gates

- Controlled paired saturation/query/transport experiments and release defaults.
- Full field inventory, eligibility screening and comparable frontier benchmarks.
- Higher-user five-minute holds, prescribed maximum search and target-kernel proof.
- Broader randomized protocol coverage and real storage/ENOSPC failure tests.
  The passed boundary injection tests do not reproduce every physical disk fault.
- Vectored/zero-copy send experiments and optional issuer/writer architectures
  when supported by profiling.
- Hardware profiling was attempted and denied (perf_event_paranoid=4);
  do not change host perf settings to obtain them.

These historical records preceded PR publication. No deployment has been performed.

## Latest qualification before submission

The published candidate keeps the same nine source files and build/start scripts
as the successful 275k five-minute artifact. Read-only image provenance verifies
their hashes. Compact evidence is committed in
[qualification-evidence.json](research/qualification-evidence.json) and the
[unchanged-k6 summary](research/bench-275000-k6-summary.json).
Focused native validation additionally passed fourteen JWT, FK, duplicate-like,
WAL and integrity cases. See [final-validation.md](research/final-validation.md).

Recent 275k short screens used 60s ramp/30s hold/no warmup and the same server
limits. A default-generator pair failed latency for both NASM #75 (p99 3,583.6ms)
and this candidate (p99 4,562.4ms, 647 HTTP/check failures, 0.0276%). An instrumented
replay passed with p99 36.3ms and zero ENOBUFS/input/output-budget failures.
This variance does not establish its cause or invalidate the separate exact
five-minute observation; short holds weight ramp transients differently.

With generator GOMAXPROCS=12 and GC tracing, both this candidate and pinned
Rust #82 passed the matched short point with zero failures:

| Submission | p95 ms | p99 ms | CPU us/request | Peak server MiB |
| --- | ---: | ---: | ---: | ---: |
| NASM candidate | 84.318 | 429.591 | 29.670 | 1,418.7 |
| Rust #82 | 72.995 | 559.034 | 27.316 | 1,391.2 |

CPU includes ramp/accept/close work. These single short pairs are not maximum
scores; Rust retains lower median latency and CPU. No whole-field superiority
is claimed. Detailed local records remain in ignored results/, with context in
[resumed-qualification.md](research/resumed-qualification.md).

Parallel timer/retry/literal/integer proposals passed focused isolated tests.
They are not applied to this submitted candidate and have no qualified capacity
advantage. The measured submission is the stable baseline.
