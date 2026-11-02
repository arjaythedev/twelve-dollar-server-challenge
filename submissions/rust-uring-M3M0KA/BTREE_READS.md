# Feed B-tree read path

The feed fast path adapts the direct SQLite B-tree technique in the challenge's [#1 Lean submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/main/submissions/lean-derive-hargup). A C shim is included in the same translation unit as the pinned SQLite amalgamation so it can use SQLite's private cursor API. SQLite still owns the pager, WAL, transaction, and locks. Rows and like counts are read live for each feed request.

## Safety and fallback

Startup checks the expected table/index layout, then scans posts to verify dense row IDs, nondecreasing timestamps, and existing authors. A feed transaction checks SQLite's data version and opens cursors only when a feed is requested. Writes close the cursors. A successful create updates the checked ID/timestamp invariant before another feed can use the direct path. External writes, unsupported layouts, failed invariant checks, and B-tree read errors fall back to the existing SQL queries. Single-post reads stay on SQL: direct-decoder trials were flat (−0.4% median) and added private-API code without a measured gain.

## Focused local comparison

An alternating A/B check compared the original `rust-epoll-scooter1337` source with this candidate's release/PGO build. The original was built in a scratch copy with the same pinned SQLite source. The candidate's synthetic PGO trainer now exercises the B-tree feed path as well as representative SQL operations. Each run used a fresh copy of the 500,000-post seed, the same Rust 1.94.0 toolchain, one server process pinned to CPU 2, and four persistent-connection Python clients pinned to CPUs 3–6 (16 connections each). Trials had a 3-second warmup and 10-second measurement; three trials per variant alternated order. The server used epoll because this container's seccomp policy denies `io_uring_setup`. Both binaries returned zero client/status/body-shape errors.

The runner and client are in [`bench/btree_ab.py`](bench/btree_ab.py). Given the two release binaries and the seed, it repeats the fresh-copy alternating comparison, records binary/seed hashes and per-run logs, and writes JSON outside the source tree by default. For example:

```bash
python3 submissions/rust-uring-M3M0KA/bench/btree_ab.py \
  --baseline /path/to/original-rust-binary \
  --candidate submissions/rust-uring-M3M0KA/target/release/twelve-rust-poll \
  --seed seed/feed.db --out /tmp/btree-ab.json
```

| Endpoint | Original median req/s | Candidate median req/s | Change |
|---|---:|---:|---:|
| `GET /feed` | 101,593 | 144,069 | +42.0% |
| `GET /posts/500000` | 257,069 | 249,547 | +1.7% |

These are directional results from a 20-vCPU KVM host with an Intel Core i5-13600K. The server was pinned to one CPU, but this run did not impose the challenge's 2 GiB memory limit or CPU quota. The custom Python client is not the official load generator, and these results do not establish a leaderboard score or capacity. Changes are medians of paired trial ratios, not ratios of the independent medians shown in the table. Post-read pairs ranged from −3.7% to +2.6%, a noisy, effectively flat result. The B-tree-specific earlier result was also flat, so post reads remain on SQL.
