# Rust / native SQLite read plans / io_uring

Direct HTTP/1.1 on `HOST:PORT` (`0.0.0.0:80` by default), without Nginx.
Rust 1.94.0, SQLite 3.54.1, pinned Cargo dependencies. Linux 6.1+ with io_uring enabled.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge bash start.sh
```

The main idea comes from [hargup's Lean-derived C submission, #153](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/153):
execute known read plans through SQLite's own B-tree cursors, skipping the SQL
bytecode interpreter. The small C bridge exposes opaque cursors from the pinned
amalgamation. Record decoding, query plans, HTTP, JSON and authentication are Rust.

- Feed ordering is read from `posts_created_at_id_idx` on every request. No startup
  table scan, dense-ID assumption, remembered ordering invariant or result cache.
  Nearby IDs share a live likes-index range scan; sparse IDs use individual seeks.
  Consecutive descending IDs walk the posts cursor backwards. Both plans handle
  timestamp ties, gaps and out-of-order timestamps.
- One issuer drives all io_uring rings. Multishot accept/receive, fixed descriptor
  tables and provided buffers support more connections than ordinary process FDs.
  Idle connections retain no response allocation. Sends retain ownership until
  their final completion; generations reject completions for reused slots.
- A completion batch shares a SQLite transaction. Writes use prepared SQL with
  foreign keys, WAL and `synchronous=NORMAL`. Responses are submitted only after
  successful commit; failed commits replace tentative replies with errors.
- SQLite's own 16 MiB page cache serves reads with mmap disabled, avoiding repeated
  WAL lookups on the mapped-page path. No data is cached in application state.
- A separate connection performs SQLite's own PASSIVE checkpoints. There is no
  custom storage engine or VFS, schema change, application data cache, CPU affinity
  or kernel tuning. The process may raise its soft descriptor limit to its inherited hard limit.
- HS256 signatures and expiration are checked on every authenticated request.
  LUFP validates JSON; borrowed fields and reusable buffers avoid ordinary request
  allocations. SSE2/NEON escaping and a bounded unsafe writer reserve once per post.

Schema/index mismatches and unsupported io_uring operations fail explicitly.
Dependencies and the SQLite archive are pinned; the downloaded archive is checksum-verified.

Validation from the challenge root:

```sh
bash test/run.sh submissions/rust-native-scooter1337
python3 submissions/rust-native-scooter1337/tests/native_oracle.py \
  submissions/rust-native-scooter1337/target/release/twelve-rust-native schema.sql
```

The SQL oracle covers sparse IDs, unordered/tied timestamps, overflow records,
escaped UTF-8, committed writes and recovery after process termination.
The final candidate passed all 42 repository checks, 11 targeted Rust tests,
six feed checks and 96 protocol/recovery checks, including 15,000 simultaneous
connections and reuse after 66 seconds idle.

One-minute mixed tests with the same two-thread client measured 51,418 req/s
versus 45,413 for the previous local Rust version. Scaling #82's 264,000-user
score by that ratio gives an expected **about 299,000 users**:
`51,418 / 45,413 * 264,000 = 298,909`. This is a rough projection; the baseline
includes local edits and the short throughput tests do not establish a user score.

Local comparison on MEGASERVER, one CPU / 2 GiB per server: a 60-second mixed
`wrk` run at 1,024 connections produced 56,260 req/s versus 49,219 for the pinned
C reference (14.3% higher). Saturated p99 was 410 ms versus 287 ms; total cgroup
peak memory was 1,753 versus 1,655 MiB, mostly reclaimable filesystem cache.
The unchanged `bench/load.js` passed at 100,000 users with a 60-second hold:
p95 203 ms, p99 347 ms, zero failed requests. These are local diagnostics,
not an official five-minute max-user score or leaderboard ranking.
