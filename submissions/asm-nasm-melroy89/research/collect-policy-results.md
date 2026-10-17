# Collection-policy screen results

These are local variant screens, not five-minute qualification results. The
candidate only changes completion collection: `--collect=round` skips a timed
wait when the current ring already has CQ work and allows no more than one
timed wait per full owner sweep. Transaction boundaries and request semantics
are unchanged. The candidate remains experimental; do not change the packaged
default on this evidence alone.

## Method

- Unchanged `bench/load.js`, SHA-256
  `14fbbe36a7bef2cc803c8c2309b8b93d6e6ec9c2b6aba78ebe3b9a95ebf85972`.
- Same AMD Ryzen Threadripper 7960X host, Linux `7.0.0-38-generic`, k6
  `1.4.2`, private shared-loopback network, source IPs `127.0.0.2-127.0.0.17`.
- Server limits: 1 CPU, 2 GiB memory, no swap, `nofile=65535`; generator
  cap 92 GiB, `GOGC=50`, `GOMEMLIMIT=80GiB`.
- Each scored screen used 275,000 VUs, 2-minute 1,000-VU warmup, the official
  60-second ramp, 60-second hold and 30-second ramp-down. The short hold makes
  these useful for screening only.
- Baseline image `sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16`,
  binary `e511302626f6f0e06c2981aeb19e4d90923afeebf497ff711373d791f9a8491e`.
- Candidate image `sha256:a1e1a161abaf7208eae7ff78f09d3682534328e9643570e4d2cf2329791c272b`,
  binary `fac4d64f2cd7f5493a450f1af4e94e3427dbf2b4e2ee033cbaafa366c544c677`.
  It uses the same pinned SQLite 3.54.0 object as baseline; image identity
  records all nine source hashes and the object hash.

## Results

| Pair | Policy | p95 | p99 | Requests/s | HTTP failures | k6 result |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| A | baseline | 804.0 ms | 8,864.3 ms | 19,367.6 | 0.0408% | failed thresholds |
| A | round | 39.6 ms | 1,080.2 ms | 20,106.2 | 0 | failed p99 threshold |
| B | baseline | 57.0 ms | 823.0 ms | 20,193.9 | 0 | passed |
| B | round | 17.9 ms | 107.3 ms | 20,415.3 | 0 | passed |

Pair A is a substantial baseline outlier, so it must not carry the conclusion.
Pair B repeated the direction: `round` reduced p95 by 68.6% and p99 by 87.0%,
with 1.1% more requests/s, while both variants passed all k6 thresholds and
all status checks. Pair B server peaks were 1.46 GiB for baseline and 1.46
GiB for `round`. Generator peaks were about 68.1-69.1 GB and none of the
generator containers was OOM-killed.

An earlier 30-second-ramp screen produced multi-second latency and is retained
as a diagnostic artifact only; its ramp differed from the challenge workload.

## Interpretation and next gate

The repeated short-screen result supports a longer test, not adoption or a
claim of higher maximum capacity. Run a matched five-minute baseline/candidate
pair at 275,000 users with the same generator controls, then confirm the best
policy with the unchanged packaging path and API suite. If the gain persists,
test higher user counts and retain five-minute holds for capacity claims.

Artifacts are `results/collect-{baseline,round}-275k-screen60*` (warmup and
scored k6 summaries, harness evidence, generator resource samples and logs).
The raw k6 exit code 99 in Pair A means a workload threshold failed; generator
OOM state is false. No winner claim follows from these screens.

## Five-minute matched hold at 275,000 users

A subsequent matched pair used the unchanged workload, 60-second ramp,
five-minute hold and standard 30-second ramp-down, with no separate warmup.
Both runs used the same 92-GiB generator cap, `GOGC=50`, `GOMEMLIMIT=80GiB`,
server limits and private loopback setup as the short screens.

| Policy | p95 | p99 | Requests/s | HTTP failures | Server peak | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 61.3 ms | 7,976.4 ms | 25,092.6 | 0.0205% | 1.87 GiB | failed p99 |
| round | 100.1 ms | 1,406.1 ms | 25,310.1 | 0.0105% | 1.63 GiB | failed p99 |

At this hold duration, `round` improved p99 by 82.4%, request rate by 0.9%,
failed requests by about 48%, and peak server memory by about 13%. Its p95 was
63% higher than baseline, though still under the 500 ms threshold. Both runs
failed the 1,000 ms p99 threshold, so neither establishes a passing 275,000-user
score under this generator configuration. This is evidence of a meaningful
tail-latency/resource improvement, not yet a passing-capacity increase.

The earlier interrupted warmup attempt `collect-baseline-275k-5m` is invalid:
k6 logged receipt of a termination signal and exited 105; Docker recorded no
OOM. It produced no scored run and is excluded from the tables above.
