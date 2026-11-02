# TODO

## Next, in order

- [ ] Split transport into parser, connection, ring and event loop; verify behavior, then run larger load groups.
- [ ] Separate bounded receive draining from handler work, then compare multishot receive and accept depth.
- [ ] Measure WAL/checkpoint costs, then compare background checkpoints and bounded group commit. Send write responses after commit.
- [ ] Compare ID-ordered post/author reads, then use the next profile to choose smaller changes such as hybrid SEND and JSON scanning.

## Later

- [ ] Test capacity on a comparable Linux VM with a separate load generator and the official file limit.
- [ ] Finally, use git mv to put our source and docs in `submissions/<stack>-SmolBlackHole/`. Rename SUBMISSION.md to README.md, update paths and run checks. Keep generated output out of the submission.

## Done

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
