# TODO

Reference: `d297ca9`, 128 CQEs / 8 ms, 4-ms receive probes and 16 writes per transaction. Use 350k VUs, seed `20261104`, a fresh seeded database and 30s ramp. Profile for 5 minutes; short comparisons use 60s hold.

- [x] Capture the [five-minute profile](src/runs/d297ca9/baseline-full-profile-350000-5m-analysis/summary.json): 217,114 CPU samples, no lost records, 20.56 us/request and zero HTTP errors. Recover the saved data after the original decoder timeout
- [x] Keep cap 32 after the 16/32-write comparison: 3.2% less CPU and 2.4% less kernel CPU in the short pair. The historical full-profile reference remains cap 16
- [x] Test cross-turn write accumulation at cap 32 ([0/1/0-ms comparison](src/runs/79cd8ce/write-window-comparison/summary.json)): 21.30 / 21.13 / 20.52 us/request. No demonstrated CPU gain; remove the deadline path and keep end-turn flushing. Off/profile lifetime, ordering, error and shutdown tests passed
- [x] Compare bounded WAL-write coalescing ([350k pair](src/runs/79cd8ce/wal-coalescing-350000-comparison/summary.json)): 20.78 to 20.40 us/request (-1.84%), kernel CPU -2.04%, p99 20.11 to 20.28 ms, zero errors. Keep the candidate; five native contracts passed, including commit visibility, rollback, errors and eviction
- [ ] Compare direct B-tree post reads later: the complete route uses 4.5% of long-profile CPU, including necessary traversal and output
- [ ] Compare checkpoint notification cadence and non-repeated eviction ranges separately if their measured costs justify them; advice uses only 0.72% of long-profile CPU
- [ ] Screen parameterless-statement cleanup and create-only borrowed body bindings separately in native probes; their sampled cost is tiny
- [ ] Keep persistent satellite notification polling for later; wakeup handling is no longer a measured hotspot after READ

Screen small SQL changes with focused tests and fixed-work native probes. Use one short matched 350k A/B with perf off for promising changes, changing one mechanism at a time.

Record user/kernel CPU per request, useful work, commits, RAM, errors and p99. Reuse controls only while source, workload and instruments match. Repeat only to resolve an inconclusive result; capacity tests follow a retained gain.

## Later

- [ ] Check socket/heap/pool memory after draining, then allocation reuse and socket-buffer caps in Ideas.md order
- [ ] Split foreground COMMIT stalls into sync/write/retry time and waiting before receive service; compare multishot receive afterward
- [ ] Test capacity on comparable Linux hardware with a separate generator and the official file limit
- [ ] Finally move our source/docs into `submissions/<stack>-SmolBlackHole/` and rename SUBMISSION.md to README.md

Keep our code in `src/`, use English and strict typing, and leave upstream files and other submissions alone
