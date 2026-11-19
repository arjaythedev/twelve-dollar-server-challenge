# Rust / raw io_uring / SQLite

## Baseline and provenance

This candidate is derived from the MIT-licensed `rust-epoll-scooter1337` implementation in
this challenge repository. Its checked-in source includes later Rust/io_uring tuning work than
the commit used for its published official score; the leaderboard recorded 264,000 users for
the earlier scored version. That result is evidence for the design, not a score for this copy.
No official capacity result is claimed for this submission.

Direct HTTP/1.1 on $HOST:$PORT. No Nginx, HTTP framework, async executor or ORM. It probes the io_uring setup and prefers that backend, falling back to epoll if the kernel denies the required setup. The Rust package and executable name are retained from the source implementation.

## Build and run

~~~bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
~~~

start.sh runs in the foreground using only the four environment variables in SPEC.md. Port 80 needs CAP_NET_BIND_SERVICE. Startup opens the existing database and verifies the fast feed plan's assumptions in one read-only scan; it does not copy table contents.

## Implementation

- Rust 1.94.0, edition 2024, native instructions, fat LTO and one codegen unit. Dependencies are pinned in Cargo.toml and Cargo.lock.
- SQLite 3.53.4, checksum-verified official source and raw C bindings. Prepared statements cache plans only. One NOMUTEX connection is held under the same mutex for every query/commit/checkpoint batch. SQLite is compiled with SQLITE_THREADSAFE=0; workers never enter it concurrently.
- Feed reads use a narrow C shim compiled in the same translation unit as SQLite's pinned amalgamation. It walks SQLite B-tree cursors and decodes live records, while the pager, WAL, locks, and transaction boundaries remain SQLite-owned. Cursor setup is deferred until a feed is requested. Startup verifies the expected schema, dense post IDs, nondecreasing timestamps, and valid authors. The feed fast path runs only while those assumptions hold; schema mismatches, external writes, and unsupported layouts use the SQL path. Writes close cursors, and a new feed scan opens fresh ones against the live transaction. This is based on the technique in the challenge's #1 Lean submission; it is not a score claim.
- io_uring issuers use multishot direct accept/receive, fixed socket descriptors, provided buffers and batched completions. Tables adapt to the inherited hard limit: eight rings of 65,535 slots under the spec limit, or one ring of 524,280 slots with a one-million limit. Accepted sockets occupy those tables instead of ordinary process descriptors, lifting the previous connection ceiling under LimitNOFILE=65535. The process raises only its own soft limit within the inherited hard limit.
- Each ring has 4,096 receive buffers and submission entries, with 16,384 completion entries. Completed buffers are published before an intermediate full-queue submission. Depleted receives wait in a bounded retry queue; retries run after buffers are returned and responses are flushed, with budgets based on receive completions instead of resubmitting every ENOBUFS immediately.
- Completion waits target 32 entries with a 250 μs maximum at sustained load, or wait for one completion when idle. These are I/O batching controls: successful writes commit before their responses are submitted.
- One 64-byte connection record. Partial inputs allocate on demand. Output allocations remain fixed until the last send completion, then return to a bounded pool. Idle connections retain no response buffers. Generations reject stale completions after slot reuse; backpressure pauses reads until output drains.
- The SQL feed fallback discovers the newest 20 IDs. If their span is under 256, live range scans fetch rows and like counts, then restore timestamp/ID order. Sparse IDs use the original query. No response cache or changed indexes.
- Live reads share a deferred SQLite transaction through each serialized completion batch. Writes upgrade it as needed, avoiding repeated pager setup while keeping every query live. Grouped responses wait for successful commit; failed batches replace tentative replies with errors.
- WAL, synchronous=NORMAL, foreign keys, exclusive locking, a 500-page SQLite cache and a 1 GiB mmap limit. Linux builds enable SQLite's fdatasync path. Synchronous RESTART checkpoints run after commit once the WAL reaches 4,096 frames, amortizing checkpoint I/O while retaining commit-before-response ordering.
- SQLite PGO trains SQL operations and the feed B-tree path on a disposable synthetic file database at build time. It uses no challenge seed data and is not linked into the server. Disable it with bash build.sh --no-default-features.
- httparse handles HTTP framing. A standalone LUFP-style cursor validates complete JSON documents and extracts JWT/body fields in one pass. Ordinary strings borrow the input; escapes and Unicode are checked before use. The x86-64 input scanner uses bounded SSE2 loads. serde and serde_json are development dependencies for differential tests only.
- hmac/sha2 verifies every HS256 signature; JWT decode and response buffers are reused. itoa formats integers, and checked SSE2/NEON loads handle response JSON escaping.

There is no application CPU affinity or kernel tuning. Every request reads SQLite while it is served. Idle keep-alives remain open; stalled partial requests or sends are shut down after 120 seconds.

Unsafe code covers C bindings, Linux I/O, vector loads and compact owning buffers. Received bytes are borrowed after their CQE transfers ownership and republished only after parsing finishes. Send allocations cannot grow, move or be freed while the kernel holds them. The connection table can grow because SQEs reference separate allocations, never connection-record addresses.

## Validation and measurements

The candidate builds with Rust 1.94.0. At startup it probes the io_uring setup and sparse-file
registration used by its workers. This validation container denies `io_uring_setup` with `EPERM`,
so the normal `start.sh` automatically selected epoll; `bash test/run.sh` then passed the official
suite **42/42** against a fresh seed. Rust unit tests passed **6/6** (4 environment-dependent
tests ignored), and `tests/check.py` passed **96** additional protocol/recovery checks, including
15,000 live sockets and reuse after 66 seconds. After adding the direct B-tree path, all three
checks and the six dense/sparse feed-range checks passed again. These verify shared HTTP/API
behavior and the epoll transport, not the io_uring transport or this candidate's capacity score.

On 30 October 2026, this release binary also passed the unchanged official `bench/load.js` at 15,000 users on the validation host: a 1,000-user/two-minute warmup followed by a five-minute hold on the same fresh-seed server and database. With k6 2.3.0, the hold recorded 560,277 requests, zero failures, p95 0.659 ms, and p99 1.15 ms. This host had no 1-vCPU/2-GiB cgroup, and seccomp denied io_uring, so the candidate used its epoll fallback. This is local validation, not an official capacity score.

The direct B-tree read implementation and its limitations are documented in [BTREE_READS.md](BTREE_READS.md). The LUFP parser follow-up and its Ubuntu/WSL measurements are documented in [LUFP.md](LUFP.md). The subsequent checkpoint, issuer, SQL and batching experiments, including rejected versions and the checkpoint tradeoff, are documented in [RESEARCH.md](RESEARCH.md).

The 4,096-frame checkpoint follow-up improves mixed throughput in Ubuntu/WSL normal-build comparisons: +7.96% across six pairs at 64 connections and +14.03%/+14.58% in two pairs at 1,024. It costs about 14 MiB more peak cgroup memory and longer checkpoint stalls; CPU/request rises at 64 connections and falls at 1,024. The normal build passes the unchanged official five-minute hold at 15,000 users with zero failures. These are local measurements, not an official capacity score.

The results below cover the read-transaction follow-up at `bd517fa`, before the buffer/retry changes above. The preceding io_uring measurements and profiles are in [IO_URING.md](IO_URING.md); older epoll comparisons and k6 searches are in [BENCHMARKS.md](BENCHMARKS.md).

On the native Xeon E5-2690 v3, with one server CPU, 2 GiB/no swap and the spec's 65,535 descriptors, the read-transaction/fdatasync follow-up compares against `b58c862`. Feed, post and creation use four balanced 15-second trials after five-second warmups; likes use six balanced 20-second trials after ten-second warmups. All use 64 connections and fresh seeds.

| Workload | Previous req/s | Current req/s | Change |
|---|---:|---:|---:|
| Feed | 32,558 | 40,097 | +23.2% |
| Single post | 122,971 | 157,398 | +28.0% |
| Create | 11,703 | 13,664 | +16.8% |
| Like | 92,607 | 93,511 | +1.0% |

Feed and post improve in every pair. Creation has one −0.75% pair; likes have two small negative pairs, so the like gain is within run-to-run variation. Socket, status and semantic errors are zero.

At 1,024 connections and the same spec resource limits, six balanced 20-second mixed trials after ten-second warmups measure 44,470 → 47,420 req/s (+6.6%). C #26 (`35cc55c`) measures 43,334 req/s; current Rust is 9.4% higher by independent medians and wins five of six paired C comparisons. The Rust-before paired median is +4.1%, with individual pairs from -7.8% to +27.1%; clock variation prevents a guarantee that every run improves. Mixed RSS is 40.09 → 41.06 MiB (C 35.88 MiB).

A separate one-million-descriptor check uses one ring, 64 connections and four balanced 20-second trials after ten-second warmups: feed 32,815 → 40,317 req/s (+22.9%), mixed 38,225 → 42,023 (+9.9%). Every pair improves; this supplemental configuration is separate from the spec comparison.

Unchanged `bench/load.js` passes a five-minute hold at 100,000 users after its 1,000-user warmup, with the same spec limits: zero failed requests/checks, p95 9.0 ms, p99 23.5 ms and a 460 MiB server cgroup peak including page cache and kernel memory. This is a confirmation on the Xeon, not a maximum-user search or the official droplet score.

From the challenge root:

~~~bash
bash test/run.sh submissions/rust-uring-M3M0KA
~~~

From this submission directory, the client can use 1M descriptors while the server is checked at the spec limit:

~~~bash
ulimit -n 1048576
python3 tests/check.py --seed /path/to/feed.db \
  --server target/release/twelve-rust-poll \
  --connections 80000 --server-nofile 65535
python3 tests/feed_ranges.py --seed /path/to/feed.db \
  --server target/release/twelve-rust-poll
~~~

The ignored allocation assertion measures 100 warmed ordinary requests per endpoint, including parsing, JWT verification, SQLite calls, commit and checkpoint. A separate ignored test checks read-to-write transaction upgrades and committed data after reopening. SQLite's C allocator and socket I/O are outside the counter. Escaped strings, chunked bodies, partial input, growth and cold connection setup can still allocate. Use a disposable seed copy because it writes data:

~~~bash
allocation_fixture=$(mktemp -d)
cp /path/to/feed.db "$allocation_fixture/feed.db"
SQLITE_PATH="$allocation_fixture/feed.db" \
  CHECK_TOKEN="$(jq -r '.[0].token' /path/to/tokens.json)" \
  RUSTFLAGS="-C target-cpu=native" \
  cargo test --release --locked -- --ignored --nocapture
rm -rf "$allocation_fixture"
~~~

Diagnostic controls add no environment variables; start.sh uses their defaults:

~~~bash
target/release/twelve-rust-poll --uring-batch=1
target/release/twelve-rust-poll --uring-wait-us=0
target/release/twelve-rust-poll --no-group
target/release/twelve-rust-poll --epoll
target/release/twelve-rust-poll --epoll --spin-us=50
~~~

The epoll path is retained for comparisons. Earlier busy-poll tests showed no reliable gain. Branchless and unchecked-index experiments are documented separately in IO_URING.md.
