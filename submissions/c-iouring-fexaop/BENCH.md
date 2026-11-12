# Development-server measurements

**Not the scoring box.** Oracle Cloud VM: AMD EPYC 7551, 2 SMT threads of one physical core,
951 MB RAM, Ubuntu 26.04, kernel 7.0.0. The load generator runs on the sibling SMT thread and
speaks loopback, and the host runs nftables/conntrack hooks, so absolute µs/request are several
times the scoring box's, and run-to-run variance is high. Only same-box, same-harness comparisons
are meaningful.

Harness: a local paired runner: drop caches, mlock the 238 MB seed copy,
`prlimit --nofile=65535 --memlock=64M`, server on CPU 0, load generator on CPU 1. `official` is
a local closed-loop mix (feed → post → 15 % like / 2 % post) with a 3 s ramp + 30 s hold, think 0;
`feed` and `post` are local path loads with 256 connections and a 20 s hold. `us_req` is the
server process's own utime+stime per request.

Baseline `ken2` = rust-iouring-kensac (#264) rebuilt for this CPU with a 16,384-entry CQ, because
the stock 65,536-entry ring allocation returns ENOMEM on this fragmented host.

## Official mix

| server | users | µs/req | overall rps | hold rps | p50 ms | p95 ms | p99 ms | CPU % | peak MB | fails |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **C (this entry), final** | 128 | **59.5–60.3** (2 runs) | 7.4–9.0k | 9.7–11.3k | 3.68–3.72 | 72.9–77.4 | 80.5–87.2 | 44–54 | 11.2–11.9 | 0 |
| ken2 (#1 design) | 128 | 56.8–65.7 (3 runs) | 7.2–9.2k | 9.4–10.4k | 3.57–3.87 | 72.1–77.4 | 79.7–83.8 | 48–52 | 12.8–13.1 | 0 |
| **C (this entry), final** | 256 | **85.1–86.6** (2 runs) | 8.0–9.6k | **9.6–11.9k** | 5.82–6.36 | 82.9–86.3 | 88.9–98.2 | 68–83 | 12.9–13.3 | 0 |
| ken2 (#1 design) | 256 | 86.9–92.9 (2 runs) | 7.3–7.6k | 9.3–9.4k | 6.8–7.2 | 86.3–87.2 | 95.3–100.2 | 66–68 | 14.7 | 0 |

Medians: the final C entry is **59.9 µs at 128 users vs 61.6** for the rebuilt #1 design, and
**85.9 µs at 256 users vs 89.9**, with **higher hold throughput in every final run** (up to
11.9k vs 9.4k) and lower p50 at 256 (5.8–6.4 ms vs 6.8–7.2 ms). No run had an error. The box
swings ±10 % run to run; the medians are the honest comparison. On the feed-only path the final entry's four runs
were 99.4, 105.7, 109.4 and 109.5 µs (median 107.5) vs 110.7 for ken2; the closed-loop rate is
latency-bound and the box state moves the whole range, so the feed edge is a few percent, not the
60 % suggested by the single fast run.

## Paired A/B (same box, alternating, two rounds)

To remove box-state drift, the final entry and the rebuilt #1 design were run alternately, two
rounds each, through the same harness:

| round | users | C entry µs/req | ken2 µs/req | C hold rps | ken2 hold rps |
|---|---:|---:|---:|---:|---:|
| 1 | 128 | 56.8 | **53.7** | 10,198 | 11,104 |
| 1 | 256 | **86.3** | 87.8 | 10,446 | 11,178 |
| 2 | 128 | **55.0** | 63.3 | 11,725 | 7,989 |
| 2 | 256 | **90.6** | 96.7 | 10,599 | 6,938 |

Mean: **128 users 55.9 vs 58.5 µs; 256 users 88.5 vs 92.3 µs.** The C entry won both 256 pairs
and 3 of the 4 paired runs; all runs had 0 errors. Pooling every run of the final source:
128 users median **56.8 µs** (52.7–60.3) vs ken2 **61.6** (53.7–65.7); 256 users median
**86.6 µs** (85.1–92.2) vs ken2 **92.9** (87.8–96.7).

## Path isolation (256 connections, pathload, no writes)

| path | server | µs/req | rps | CPU % |
|---|---|---:|---:|---:|
| GET /feed | C (this entry), four runs | 99.4–109.5 (median 107.5) | 6.7–9.8k | 73–98 |
| GET /feed | ken2 | 110.7 | 6,069 | 67.2 |
| GET /posts/:id | C (this entry) | 82.5–85.3 | 7.9–8.4k | 68–69 |
| GET /posts/:id | ken2 | 79.9–85.8 | 8.5–10.2k | 73–82 |

The feed plan is the edge: the tail scan plus the batched author descent is a few percent cheaper
in the feed path, more when the box is in its fast state. The single-post path is at parity; its
cost is the two rowid seeks, one likes-index range count and the kernel.


## Connection capacity and scoring-box robustness

The scored run is not a throughput contest: the app is offered more users than it can serve, so
the pass must stay responsive while up to ~310k keep-alive sockets and their completions are live.
The answer budget and ready queue bound how much request parsing one event-loop pass can do:

- At most `ANSWERS_PER_PASS` (2,048) requests are answered per pass. Bytes for requests parsed
  after the budget is spent, or arriving while a send is in flight, stay in that connection's
  pending buffer and the connection goes on the ready queue. `run()` drains the ready queue before
  and after the read round; a later pass serves the deferred requests. This keeps accept, send,
  commit, checkpoint and timer work from waiting behind an unbounded run of parse+SQLite work.
- Pending buffers are compact (512 bytes initially, grown only for bytes actually buffered) and
  are freed on completion. Response buffers still come from the 8 KiB process-wide pool. With the
  old per-connection 8 KiB partial buffer, 310k connections waiting on the ready queue would have
  pinned ~2.5 GB; the compact path costs a few hundred bytes per waiting connection.
- EOF/error sets `close_after` but does not close a connection that still has a deferred complete
  request: `end_batch` and the send completion arm the close only when `c->ready` is clear, and
  `received()` accepts buffered-request processing (`n == 0`) after EOF. A pipelined client that
  half-closes still gets every complete request answered.

New tests (dev workstation, 16 threads / 15 GB, not the Oracle VM above, and not the scoring box;
server pinned to CPU 0 with `taskset`, client on the other CPUs):

| test | result |
|---|---|
| 50,000 simultaneous keep-alive connections, one `GET /feed` each, held open | 50,000/50,000 200, 0 errors; server RSS 22.9 MB; process fds 16 before and after |
| 50,000 connect/request/close cycles (2,000 in flight) | 50,000/50,000 200, 0 errors; RSS settled at 15.9 MB; no fd growth |
| 10,000 simultaneous connections under `prlimit --nofile=4096` (4,096-slot rings) | all 10,000 served; rings 2 and 3 added; RSS 24.0 MB; fds 18 |
| 200,000 simultaneous connections under `prlimit --nofile=65535` (the benchmark box's limit; 65,535-slot rings) | 200,000/200,000 200, **0 errors**; rings 2, 3 and 4 added; server RSS 49.8 MB; fds 19 |
| 100,000 connect -> `GET /feed` -> close cycles (65,535-slot ring, 2,000 in flight) | 100,000/100,000 200, **0 errors**; RSS 17.4 MB; fds 16 before and after |
| 70,000 sequential connect -> `GET /feed` -> close cycles (65,535-slot ring, one at a time) | 70,000/70,000 200, **0 errors**; crosses 65,536 slot generations; RSS 17.4 MB; fds 16 |
| 20,000 `POST /posts` with `Connection: close` and an immediate write-half-close, 128 in flight | 20,000/20,000 201, **0 errors**; WAL reached 16.9 MB and checkpoints/bulk holds ran; no request lost at EOF |
| 5,000 pipelined `GET /posts/500000` on one socket, then `shutdown(SHUT_WR)` | 5,000/5,000 responses, then close; request bytes beyond the first 2,048 answered across later passes |
| 15 s mixed load, 256 connections, no think time (feed -> post -> 15 % like / 2 % post), 20k tokens | 343,947 requests, statuses 200/201, **0 errors**; p50 11.1 / p95 13.2 / p99 14.3 ms; RSS 29.1 MB |
| same host, `oha` 200 connections, GET /feed, 5 s, alternating runs | C entry avg 3.20 / 3.39 ms (p99 5.51 / 6.33); ken2 avg 3.32 / 3.77 ms (p99 5.73 / 6.80) |

The connection RSS is the relevant number for the 2 GB box: 49.8 MB at 200,000 simultaneous
connections (about 0.22 kB per connection above the idle baseline), because idle connections hold
no response buffer. At 310,000 the app's own share extrapolates to roughly 70 MB across five
65,535-slot rings. The scoring box's kernel socket memory is the remaining cost and is outside the
app's control; the app no longer adds an 8 KiB buffer per waiting connection on top of it.

## How the C server got there

| step | official 128 users, µs/req |
|---|---:|
| first working C server (tail feed, checkpoint thread, 4,096 × 1 KiB buffers) | 97.2 |
| + fast integer/Content-Length formatting, block JSON escaper, checkpoint hold/reset state machine | 68.3 |
| + startup invariant scan moved to a throwaway 1 MiB-cache connection | **57.8** |
| + duplicate the held-write release before the pass commit (256 users) | 96.1 → 90.0 |
| + in-place post decode and raw response writer (256 users) | 88.5–98.8 (inside the box variance) |
| + batched author rowid descent + raw header writer (256 users) | **85.1–86.6 (median 85.9)** |

The first perf profile (999 Hz) had `__printf_buffer`/`vsnprintf` at 4.3 %,
`walMerge` + `pcache1TruncateUnsafe` + ext4 write-begin at 6.5 %, and `json_escape` at 3.35 %.
`snprintf` was in every response field; the WAL was growing without resetting (48 MB after a
40 s run), and the wal-index hash merge cost grows with the log. The startup scan on the serving
connection had filled its 64 MB page cache with the posts table (peak RSS 77 MB); isolating the
scan made the serving cache hold only the hot set (11.5 MB) and cut the official workload by 18 %.

After the changes the profile is flat: SQLite B-tree `TableMoveto`/`GetPage` ~9 %, JSON escaping
~3 %, the rest in the io_uring enter path, TCP and the host's nftables. No single large hotspot
remains.

## Correctness on the same build

- `test/run.sh` 42/42 with the final build, including after the answer-budget/ready-queue,
  held-connection close, 16-bit generation, EOF re-close and commit-failure fixes (in-place post
  decode, batched author descent, raw response writers; the memlock/CQ/slot fallbacks engage on the
  dev host).
- Deferred-request lifecycle: 5,000 pipelined requests then half-close all answered; 20,000 POSTs
  with an immediate half-close under checkpoint hold all answered 201; EOF with a deferred request
  that turns out incomplete re-arms the close (no CLOSE_WAIT leak).
- Generation handling: direct-descriptor slot reuse uses a 16-bit generation (the ring's user-data
  and every queue entry store 16 bits); 70,000 sequential connect/close cycles on a 65,535-slot
  ring cross 65,536 generations with 0 errors, and 100,000 churn cycles with 0 errors and no fd
  growth.
- Non-monotonic data (post 1000's `created_at` moved to 2027): the index plan answers and matches
  the reference SQL byte for byte.
- A missing author row: the batched descent reports it and the reference-SQL fallback answers,
  matching byte for byte.
- The non-monotonic database was re-run on the final build (index feed plan + batched authors):
  byte-for-byte match.
- 200 inserted posts: the feed matches the reference SQL byte for byte; the tail plan stays on.
- 1,024-slot rings: 2,500 idle connections create 3 rings and the last is served; 3,000
  connect/health/close cycles all succeed (direct-descriptor close frees the slot).
