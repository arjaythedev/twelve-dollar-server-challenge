# TODO

## Next, in order

- [ ] Compare the rewrite against `671fd4e`, inspect changed hotpaths in disassembly and take a short profile; investigate the higher CPU/request in the phase 4 run.
- [ ] Compare prepared transaction/savepoint statements and inspect the generated code.

## Later

- [ ] Trace delayed TCP EOF with another idle connection; reproduced through Docker's published port in phases 3 and 4.
- [ ] Split foreground COMMIT stalls into sync/write/retry time, then trace waiting before receive-CQE service and deferred reads.
- [ ] Compare multishot receive after isolating handler stalls.
- [ ] Test capacity on a comparable Linux VM with a separate load generator and the official file limit.
- [ ] Finally, use git mv to put our source and docs in `submissions/<stack>-SmolBlackHole/`. Rename SUBMISSION.md to README.md, update paths and run checks. Keep generated output out of the submission.

## Done

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
