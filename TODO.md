# TODO

## Next, in order

- [ ] Simplify dev tooling, telemetry reporting and CMake without changing behavior.
- [ ] Check socket/heap/pool memory after draining, then compare allocation reuse and socket-buffer caps in Ideas.md order.

## Later

- [ ] Split foreground COMMIT stalls into sync/write/retry time, then trace waiting before receive-CQE service and deferred reads.
- [ ] Compare multishot receive after isolating handler stalls.
- [ ] Test capacity on a comparable Linux VM with a separate load generator and the official file limit.
- [ ] Finally, use git mv to put our source and docs in `submissions/<stack>-SmolBlackHole/`. Rename SUBMISSION.md to README.md, update paths and run checks. Keep generated output out of the submission.

## Done

- [x] Compare GCC 13 PGO: CPU/request -2.9%, hold p99 5.87 to 38.71 ms at 350k. Discard the implementation and retain the results.
- [x] Measure current per-ring receive outcomes, connection counts and deferred age. At 350k: six rings, zero ENOBUFS, no CQ overflow or deferred receives in four complete full-load intervals. No balancing change.
- [x] Compare CQ rotation, then restore fixed starts after no clear gain at 350k. Keep accept/wait cleanup. Growth, receive, partial sends and shutdown pass in both builds. Matched 350k/5m B/A: zero errors, CPU/request +0.5%, hold p99 -1.2%.
- [x] Count likes per B-tree leaf, retaining bounded gaps, interior entries and fallback. Pass SQL-oracle, snapshot, signed-ID and overflow checks at 512/4096-byte pages, plus sanitizers. Native feed CPU -8.2% seed, -58.9% dense, -20.1% sparse. Matched 350k/5m A/B: CPU/request -1.9%, hold p99 6.02 to 5.86 ms, zero errors. Earlier tail variability remains unresolved.
- [x] Keep WAL eviction with limit 0: matched 350k NAT runs reduced peak RAM from 1513 to 956 MiB and hold p99 from 11.01 to 5.77 ms; both passed.
- [x] Use ext4 for Mini runs, verify the generic x86-64 build and fix eviction-induced WAL rereads with journal_size_limit=0. Pass focused tests in both build modes and 200k via VM/LAN: zero errors, hold p99 7.81 ms, peak RAM 1322.78 MiB.
- [x] Replace create RETURNING with plain INSERT and stored timestamp readback. Verify failed-readback rollback, 64-bit rowids and metadata. Pass both native builds, 42 official and two targeted HTTP checks; native batch-16 CPU/write fell 15%. Confirm at 350k/5m: no errors, p99 5.72 ms; overall CPU unchanged within run variation.
- [x] Skip the up-front like SELECT, classify FK failures and duplicates with a transaction-local B-tree lookup, and preserve SQL fallback. Pass both native builds, 42 official and five targeted HTTP checks. Confirm with unix-excl at 350k/5m: no errors, p99 5.67 ms, CPU 20.45 us/request.
- [x] Use unix-excl on both SQLite handles; verify checkpoint concurrency, recovery, fresh reads and HTTP commit ordering in both build modes. Pass 42 official and 19 extra HTTP checks. Short 70k pair: CPU/request -2.5%, p99 nearly unchanged, no errors.
- [x] Prepare rollback and quarantine unresolved transactions; verify failure injection, read/write rejection and clean reopening. Compare retry-only poll-first omission and bounded response reuse, then confirm at 350k/5m: zero errors, p99 6.82 ms. Pass both native build modes, 42 official and 17 extra HTTP checks.
- [x] Enable primary-only SINGLE_ISSUER / DEFER_TASKRUN; verify receive, partial send and stop with continuously ready satellites in both build modes. Include fixed 16-byte CQEs and the wait ring span at 300k/5m: zero errors, p99 6.39 ms, CPU 23.91 us/request. Separate native cursor/span timings remain inconclusive.
- [x] Keep CQ 8,192 after native burst/lifetime checks and a full 300k/5m pass: zero errors, p99 6.58 ms; no clear steady-load gain.
- [x] Bound load-generator timer batches and report pre-poll gaps; pass conformance checks and confirm at 300k/5m with the same server: zero errors, p99 6.37 ms.
- [x] Prepare transaction/savepoint statements and reuse 16 write-outcome slots; pass recovery/lifetime checks in both build modes, inspect assembly, compare native writes and confirm at 300k/5m: zero errors, p99 7.16 ms.
- [x] Compare earlier send submission after each ring drain at 70k; revert after no CPU or latency gain.
- [x] Bound ENOBUFS retries with returned-buffer credits and timed probes; pass exhaustion, one-refill recovery and shutdown checks in both build modes. Short 50k comparison: zero errors, unchanged p99.
- [x] Remove phase 6, retain its measurements and ideas, and confirm the restored source/binary at 300k/5m: zero errors, p99 threshold still fails.
- [x] Complete the rewrite comparison against `671fd4e`: short profiles, disassembly and matched 300k runs; no request errors, essentially equal CPU cost.
- [x] Shut down fixed sockets before unregistering them; verify prompt TCP EOF with an active idle peer, slot reuse and terminal-CQE ownership.
- [x] Separate transport diagnostics and clarify queued-write ownership; pass both build modes and confirm at 300k. The normal binary is unchanged by diagnostic extraction.
- [x] Route and authorize once, queue owned write commands and remove batch response remapping; pass native, official and HTTP lifecycle checks.
- [x] Remove the old single-write API, migrate tests to write_batch, reuse batch response storage and simplify read dispatch. Inspect assembly and confirm at 300k.
- [x] Group up to 16 queued HTTP writes without added delay; verify commit failure, savepoint isolation, response order and shutdown. Compare short and five-minute 300k runs.
- [x] Capture five-second server/client timings at 300k; identify foreground COMMIT stalls and the following CQ/CPU backlog, with millisecond client scheduling delays.
- [x] Read feed posts and unique authors in ID order within each request; verify direct SQLite results, snapshots and overflow, then compare native CPU and a 300k profile.
- [x] Compare idle multishot polls at 300k; retain pending receives and the extra half-close/send-readiness tests.
- [x] Record cgroup memory categories alongside CPU and pressure samples.
- [x] Reuse empty request buffers within the Exchange budget; verify isolation, oversize rejection and generated reset code.
- [x] Measure checkpoint stalls, add an owned checkpoint worker and compare at 100k; lower p99 costs more CPU.
- [x] Compare bounded CQ/handler queues; retain inline dispatch after no measured gain.
- [x] Keep 64 direct accepts pending; compare short bursts and verify multi-ring reuse and shutdown.
- [x] Use 56-byte connections and bounded Exchange reuse; pass native, sanitizer and HTTP checks, then compare at 100k.
- [x] Build the native load generator, compare its behavior and memory with k6 at 10,000 users and integrate it into dev.py.
- [x] Split transport into parser, connection, ring and event loop; simplify state, pass behavior checks and compare at 50,000 users.
- [x] Build and measure the typed Python baseline; replace its runtime with C++23/io_uring and keep Python for tooling.
- [x] Pass native, official and extra HTTP tests; profile user and kernel CPU.
- [x] Keep B-tree feed reads, request-local like scans and local payload decoding; retain SQL point reads.
- [x] Compare shared receive buffers, scatter-gather sends and completion batching, including lifetime and exhaustion tests.
- [x] Compare LTO, Cascade Lake tuning and GCC16 without changing the production compiler.
- [x] Compare direct receive parsing at 50,000 users; keep the current parser path for now.
- [x] Reuse HTTP header storage, skip duplicate B-tree cleanup after commit and append clean JSON runs.
- [x] Use direct descriptors across lazy rings on one thread; pass 70,000 connections under the official 65,535 file limit, including reuse and shutdown.

Keep our code in src/, use English and strict typing. Leave upstream files and other submissions alone.

Dev commands: `uv run --directory src --locked dev.py --help`.
