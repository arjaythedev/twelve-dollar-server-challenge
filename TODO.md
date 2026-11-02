# TODO

## Next, in order

- [ ] Isolate the remaining 300k p99 stalls with per-ring CQ, handler/SQLite and I/O-pressure timing; the checkpoint worker still misses p99 despite eliminating request errors.
- [ ] Compare multishot receive after isolating handler stalls.
- [ ] Compare ID-ordered post/author reads, then use the next profile to choose smaller changes such as hybrid SEND and JSON scanning.

## Later

- [ ] Test capacity on a comparable Linux VM with a separate load generator and the official file limit.
- [ ] Finally, use git mv to put our source and docs in `submissions/<stack>-SmolBlackHole/`. Rename SUBMISSION.md to README.md, update paths and run checks. Keep generated output out of the submission.

## Done

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
