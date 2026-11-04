# TODO

Reference: `d297ca9`, 128 CQEs / 8 ms, 4-ms receive probes and 16 writes per transaction. Use 350k VUs, seed `20261104`, a fresh seeded database and 30s ramp. Profile for 5 minutes; short comparisons use 60s hold.

- [x] Capture the [five-minute profile](src/runs/d297ca9/baseline-full-profile-350000-5m-analysis/summary.json): 217,114 CPU samples, no lost records, 20.56 us/request and zero HTTP errors. Recover the saved data after the original decoder timeout
- [x] Keep cap 32 after the 16/32-write comparison: 3.2% less CPU and 2.4% less kernel CPU in the short pair. The historical full-profile reference remains cap 16
- [x] Test cross-turn write accumulation at cap 32 ([0/1/0-ms comparison](src/runs/79cd8ce/write-window-comparison/summary.json)): 21.30 / 21.13 / 20.52 us/request. No demonstrated CPU gain; remove the deadline path and keep end-turn flushing. Off/profile lifetime, ordering, error and shutdown tests passed
- [x] Compare bounded WAL-write coalescing ([350k pair](src/runs/79cd8ce/wal-coalescing-350000-comparison/summary.json)): 20.78 to 20.40 us/request (-1.84%), kernel CPU -2.04%, p99 20.11 to 20.28 ms, zero errors. Keep the candidate; five native contracts passed, including commit visibility, rollback, errors and eviction
- [x] Implement one-shot B-tree post reads with fresh snapshots and SQL fallback. Six native contracts passed; the [common-case Application probe](src/runs/03180a1/btree-post-common-native/summary.json) used 1.72 to 1.34 us/read (-22.4%) with identical status/JSON
- [x] Compare direct post reads at 350k with WAL, cap 32 and CQ 128/8 ms fixed ([A/B/B/A](src/runs/6cd248e/post-btree-abba-350000/summary.json)): weighted SQL 20.51 versus B-tree 20.88 us/request (+1.79%). One B run failed p99 despite zero errors; the initial -0.74% pair was not confirmed
- [ ] Investigate the B control's tail stall: hold p99 1.30 s, max 4.87 s. The [sampled 35-54s window](src/runs/6cd248e/post-b2-window-analysis/summary.json) shows 86% CPU versus 67-69% in controls. Commit/ENOBUFS timings are unavailable; large full-run throttle/retransmit totals mostly belong to teardown. Cause remains unknown
- [x] Compare listener TCP_NODELAY off/on ([pair](src/runs/6cd248e/tcp-nodelay-350000-comparison/summary.json)): total CPU -0.03%, approximate hold CPU +0.35%, segments/request effectively unchanged, zero errors. Remove the candidate and keep the default
- [x] Screen disassembly candidates: keep required-header capture and direct reply formatting after [native comparisons](src/runs/6cd248e/http-disassembly-native/summary.json) and four focused contracts. Discard duplicate point-read initialization; its comparison was inconclusive. Full-server gains remain unconfirmed
- [x] Keep the consumed-input offset after the [native comparison](src/runs/146d5f2/session-offset-native/summary.json): body CPU -4.6%, pipeline32 -13.8%; ordinary GET and fragmented-body results inconclusive. Session +8 bytes, retained capacity unchanged; fragments, pipeline order, reset and buffer bounds passed
- [x] Compare the offset at 350k ([control/repeat](src/runs/146d5f2/session-offset-350000-comparison/summary.json)): total CPU -0.73%, approximate hold CPU +0.50%, hold p99 18.56 to 21.02 ms. Both valid runs passed; first offset attempt activated late. No full-server gain established
- [x] Remove stored header names and swap completed authorization buffers ([probe](src/runs/89b5d0c/parser-buffers-native/summary.json)): Parser 336 to 304 bytes, first auth allocation count 2 to 1, warm 0 in both; four contracts passed. Fragmented capacity rises 502 to 540; CPU results are noisy, no whole-server gain measured
- [x] Reject deferred RELEASE ([production probe](src/runs/429574d/savepoint-release-native/summary.json)): batch 32 foreground CPU +116.9% valid / +131.1% mixed; all eight pairs slower. Six contracts passed. Restore per-write RELEASE; retain production probe and new trigger-isolation tests
- [x] Reject incremental WAL advice after the [350k comparison](src/runs/429574d/wal-ranges-350000-comparison/summary.json): CPU 22.03 to 22.72 us/request, hold p99 22.77 to 408.79 ms, RAM +23.64 MiB. Native helper gains did not carry through. Initial control activated late; its valid repeat is the comparison. Keep results, restore the existing advice path
- [ ] Compare checkpoint notification cadence separately if measured costs justify it; advice used only 0.72% of long-profile CPU
- [x] Reject create-only borrowed body binding after the [native comparison](src/runs/429574d/create-body-borrow-native/summary.json): valid batch-32 CPU +12.6%, all four pairs slower; mixed results inconclusive. Lifetime/rollback contracts passed; keep `SQLITE_TRANSIENT`
- [ ] Screen parameterless-statement cleanup only if a current profile justifies it; all binding cleanup sampled just 0.037% of short-profile CPU
- [ ] Keep persistent satellite notification polling for later; wakeup handling is no longer a measured hotspot after READ

Screen small SQL changes with focused tests and fixed-work native probes. Use one short matched 350k A/B with perf off for promising changes, changing one mechanism at a time.

Record user/kernel CPU per request, useful work, commits, RAM, errors and p99. Reuse controls only while source, workload and instruments match. Repeat only to resolve an inconclusive result; capacity tests follow a retained gain.

## Later

- [ ] Check socket/heap/pool memory after draining, then allocation reuse and socket-buffer caps in Ideas.md order
- [ ] Split foreground COMMIT stalls into sync/write/retry time and waiting before receive service; compare multishot receive afterward
- [ ] Test capacity on comparable Linux hardware with a separate generator and the official file limit
- [ ] Finally move our source/docs into `submissions/<stack>-SmolBlackHole/` and rename SUBMISSION.md to README.md

Keep our code in `src/`, use English and strict typing, and leave upstream files and other submissions alone
