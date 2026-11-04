# TODO

Reference: `d297ca9`, 128 CQEs / 8 ms, 4-ms receive probes and 16 writes per transaction. Use 350k VUs, seed `20261104`, a fresh seeded database and 30s ramp. Profile for 5 minutes; short comparisons use 60s hold.

- [x] Capture the [five-minute profile](src/runs/d297ca9/baseline-full-profile-350000-5m-analysis/summary.json): 217,114 CPU samples, no lost records, 20.56 us/request and zero HTTP errors. Recover the saved data after the original decoder timeout
- [x] Record the 16/32-write comparison with CQ and timeout fixed: 32 used 3.2% less CPU and 2.4% less kernel CPU; keep 16 for the profiling reference until review
- [ ] Compare cross-turn write accumulation first: use cap 32, compare 0 and 1 ms, then 2 ms only if useful. Reuse the bounded FIFO, start the deadline at its oldest command and keep SQLite transactions closed until flush
  Bound every reactor wait, including idle/EINTR paths; check expiry during busy drains. Preserve pipeline order, commit-before-response and shutdown discard. Keep immediate flushing without EXT_ARG. Test sparse singleton expiry, capacity, busy satellites, errors and stop; record flush reason, batch size and queue age
- [ ] Compare bounded contiguous WAL-write coalescing next: foreground pwrite paths use 4.8% of long-profile CPU, an upper bound. Flush commit bytes before NORMAL visibility, preserving short writes, errors and eviction
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
