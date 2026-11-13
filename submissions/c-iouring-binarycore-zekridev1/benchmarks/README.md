# Benchmarks

Reproducible load-test scripts for BinaryCore.

- `run.sh` — run a k6 load test and write a JSON summary report.
- `profile.sh` — perf profiling notes.
- `compare.sh` — compare before/after JSON reports.

## Procedure

Follow the official competition procedure (README.md of the challenge):

1. Warm-up: 1,000 users for 2 minutes (not scored).
2. Find the limit: start at 2,500 users, doubling until a run fails, then
   narrow down to within 250 users.
3. Confirm: one 5-minute hold at that number.

Pass = p95 < 500 ms, p99 < 1 s, errors < 1 %.

Score = most users that pass a 5-minute hold.

## Example

```bash
bash benchmarks/run.sh \
  BASE_URL=http://127.0.0.1:3000 \
  VUS=2000 DURATION=60s
```

## Caveats

- These are local benchmarks on a development machine. Do not claim they are
  equivalent to the official environment (Synteq 1 vCPU / 2 GB, separate
  16-core load generators, 0.25 ms RTT).
- Do not fabricate benchmark data. If k6 is not installed, state exactly why
  the benchmark cannot run.
