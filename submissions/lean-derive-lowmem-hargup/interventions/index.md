# Metal optimization interventions

Completed 2026-10-27. Branch: `lean-derive-metal-hargup`. Baseline: `100234f` (includes WAL bound).
One unified source tree. Git commits identify experiments; no source-variant directories.
The remote tree is a mirror of the committed source. Executables and logs may be retained
by SHA for interleaved comparisons without maintaining divergent source implementations.

## Stack rank

Ranking weighs frequency, removable work, implementation cost and correctness risk. It is
not a claimed speedup. Design the shared type system before optimizing individual consumers.

| Rank | Intervention | Reason | Status |
|---:|---|---|---|
| 0 | [Shared type system](00-type-system.md) | Establish facts once and drive all lowerings | retained; all consumers use it |
| 1 | [Projected records](01-projected-records.md) | Avoid decoding unused trailing fields | P3 retained; P1/P2 superseded |
| 2 | [Endpoint claims](02-endpoint-claims.md) | Eliminate unused/duplicate materialization | C3 retained; C1/C2 superseded |
| 3 | [Bounded responses](03-bounded-responses.md) | Select one bounded allocation from shape evidence | R1 retained: 224 versus 1,056 requested bytes |
| 4 | [Job layouts](04-job-layouts.md) | Shrink the common representation | J1/S1 retained: 32 versus 72-byte envelope |
| 5 | [Execution streams](05-execution-streams.md) | Endpoint effects choose dispatch structure | retained with J1; ordered writes preserved |
| 6 | [Refined text](06-refined-text.md) | Preserve parser facts through output | T1 retained; timestamp invariant not adopted |
| 7 | [Incremental framing](07-incremental-framing.md) | Avoid repeated header work on fragments | H1 retained: ~19x targeted parsing improvement |
| 8 | [SQE publication](08-sqe-publication.md) | Publish fully constructed entries by batch | Q1 retained: ~99.2% fewer tail publications |
| 9 | [Ideas from PR #158](09-pr158-ideas.md) | General techniques from another entry, profiled first | W1 WAL write buffer (−87% `pwrite`) and A1 AVX2 escaping (−2.5% feed CPU) retained; page-cache reads and larger checkpoints rejected |

## Experiment discipline

- Write and review the shared type design before implementation. Prefer one coherent change
  when several interventions follow from one type-level fact; record the grouping explicitly.
- Commit source before measurements. Record the full SHA and executable hash in results.
  Rejected implementations stay in history and are reverted in the unified source.
- Compare against the last retained executable with interleaved runs. Application CPU and
  targeted microbenchmarks explain causes; mixed HTTP checks end-to-end effects.
- Use fresh database copies, one server CPU, 2 GiB memory, no swap, and 65,535 descriptors.
  Wait for quiet windows on bigmachine; never stop somebody else's benchmark. Record and
  exclude overlapping performance runs. Do not change system-wide settings.
- Preserve runtime input validation, SQLite reads per request, authentication per request,
  transaction/commit ordering, foreign-write fallback, and the exact wire contract.
- Rebuild Lean and check generated C. Run all 42 official tests, meaningful edge/differential
  tests, and appropriate sanitizer checks for memory/lifetime changes. Finish with clean
  Ubuntu CI and an unprivileged high-connection smoke test of the final retained source.
- Keep a change only with correctness evidence and a demonstrated performance/space benefit
  appropriate to its purpose. Record noisy or negative results honestly. No official-score
  or maximum-capacity claims from short local tests.

## Final candidate and scope of results

Production candidate: `2eae17a41220b7eb99420b62d473088b939599f1`.
Documentation/test commits through `6051565ccba5bbee82c250ce2d50ce3adf183570` have
identical `runtime/` and `gen/` trees. Baseline executable:
`93ab6ebb4fabc3af39769324f9d241ba12abb58f` (production C identical to `100234f`).

The complete candidate was compared against baseline in 30-second mixed HTTP windows,
A/B/B/A, fresh seed per run, UID 65534, one server CPU, 2 GiB, no swap and 65,535
process descriptors. Client: four threads, 32 connections each. Workload: 46% feed,
46% post, 7% like, 1% create. No external loadgen/blast/compiler overlap was observed.

| Metric | Baseline runs | Final runs |
|---|---:|---:|
| Requests/s | 175,714; 172,804 | 178,488; 178,566 |
| Server CPU µs/request | 5.685; 5.763 | 5.585; 5.601 |
| Errors | 0; 0 | 0; 0 |

Mean throughput is about **2.4% higher**, with about **2.3% less server CPU per
request** in this test. This is a modest measured gain, not an official score or a
maximum-capacity claim. Shorter earlier tests were noisy; their results remain in
the individual journals. Microbenchmarks identify work removed and must not be
read as equivalent end-to-end speedups. Total mixed-process memory is not claimed
to fall: the reported byte reductions apply to specific jobs/response allocations.

Raw mixed result directory suffixes: `1793084845183037776`, `1793084886892260156`,
`1793084928662931461`, `1793084970645549010` under the remote `metal/results` tree.

## Validation

All 26 Lean build targets and generated application/header checks pass locally.
The candidate passes all 42 official API cases, 20,000 random/deterministic SQL post
comparisons plus feed/key-order comparison, fragmented/pipelined requests, escaping,
UTF-8 and expanded authentication cases. Sanitizers cover output ownership, records,
framing allocations, SQ wrap/partial submissions and generated batch rollback/reuse.
The real Linux API suite also passes with a four-entry submission queue.

A valid small-page SQLite fixture reproduced the original shared-overflow-buffer
corruption; per-cursor ownership fixes it. Native reads and foreign-write fallback
both match SQL on that fixture. No rows, responses or verified tokens are cached
across requests.

A fresh Ubuntu 24.04 container installed the pinned/checksummed Lean and SQLite
releases, rebuilt all 26 targets and all four generated files from clean sources,
then built/started/tested as UID 10001: **42 passed, 0 failed**. Source:
`6051565ccba5bbee82c250ce2d50ce3adf183570`; production code matches the measured
candidate. [Complete CI log](results/ci-6051565.log). The container used an explicit
seccomp exception for io_uring, without privileged mode or host namespaces.

## High-connection smoke test

One baseline/final pair used 300,000 VUs, 20-second ramp, 30-second hold, 5-second
ramp-down and 10-second drain, with the same server restrictions. Both actually
reached **300,000 simultaneous connections** and passed the load generator's verdict.

| Metric | Baseline | Final |
|---|---:|---:|
| Completed requests | 1,604,004 | 1,604,052 |
| Recorded request/connect errors | 0 / 0 | 0 / 0 |
| Hold CPU µs/request | 18.216 | 14.937 |
| Hold p95 / p99 (ms) | 3.147 / 23.136 | 4.423 / 11.919 |
| Peak cgroup memory (MiB) | 515.75 | 514.84 |
| Stop-time cancellations | 53 | 38 |

This single pair shows about 18% less hold CPU per request, nearly unchanged memory,
a higher p95 and lower p99. It is a smoke comparison, not a repeated latency or
capacity study. Stop-time cancellations are reported separately by the generator
and are not included in its completed-request error count.

## Reviewable evidence

- [Collected measurements, full source SHAs, binary hashes, compiler modes and load metadata](results/measurements.json)
- [Full commit mapping](results/source-map.json)
- [Targeted measurements and sanitizer command evidence](results/targeted.json)
- [Clean Ubuntu build/test log](results/ci-6051565.log)

Raw remote artifacts remain under `/root/twelve/opt-audit-1024/metal/artifacts` and
`results`; the source mirror is a single committed tree. Reproduction entry points
are `tests/metal/build.sh`, `bench.py`, `pair.py`, `text_pair.py`, `batch_run.py` and
`ci.sh`. Individual intervention files retain rejected candidates and their evidence.
