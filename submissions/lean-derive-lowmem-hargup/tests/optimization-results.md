# Optimization trials — 2026-10-24

Experiments ran on `bigmachine` against a frozen copy of the lower-memory submission at
`0dd7e387c76fb20902ad1e5ad86cf83fd8196a29`. Only the input-buffer and cursor-storage changes
were retained from the first four trials. The concurrent WAL-checkpoint work is separate
and was not included in these comparisons.

| Trial | Decision | Evidence |
|---|---|---|
| Release drained input buffers | Keep | For 10,000 idle connections after fragmented requests, added anonymous memory fell from 40.7 MiB to 1.6 MiB. Every connection remained usable. |
| Reuse cursor storage | Keep | Median application CPU fell 2.8% for individual mixed reads and 3.9% for batches of 32. Network throughput was within run-to-run variation. |
| Open only the required cursors | Reject | Mixed reads were slightly slower; endpoint-specific gains were inconsistent. |
| Size escaped JSON exactly | Reject | A 500-character ASCII post used a 1,024-byte allocation instead of 3,479 bytes, but individual mixed reads cost 2–5% more CPU across two implementations. |
| Shorter/adaptive batching delay | Retain 250 µs | Zero/adaptive delays increased CPU substantially. The 50 µs result was near parity at 50k users and only 4% lower CPU in one 200k pair, insufficient given control variance and the concurrent checkpoint change. |
| Compile with `-O3` | Reject; retain `-O2` | No consistent gain: median CPU was 0.2% higher for individual mixed reads, 1.0% higher for batches of 32, and 2.8% higher for single posts. |

## Methods and limits

Each variant passed all 42 official checks and the additional `edge_cases.py` checks:
fragmented and pipelined writes/reads, long strings, JSON escaping, UTF-8, response key order,
and 100 pipelined health requests. Candidate tests used disposable copies of the seed.

Cursor timings use the real application batch executor and serializers, excluding networking
and JWT verification. Each comparison interleaves A/B/B/A/A/B, with three samples per
variant, and measures thread CPU time. These are application-cost measurements, not an
end-to-end capacity claim.

| Mixed read workload | Before cursor reuse | After cursor reuse |
|---|---:|---:|
| One request per batch | 4,214.63 ns/request | 4,095.52 ns/request |
| 32 requests per batch | 3,211.82 ns/request | 3,086.17 ns/request |

The memory test keeps 10,000 connections open after sending fragmented requests with roughly
3 KiB of headers. It measures the change in server-cgroup anonymous memory, excluding the
sockets' kernel memory. It models fragmented traffic, not every idle connection.

Network trials use one server CPU, a 2 GiB cgroup limit, no swap, and a 65,535 descriptor
limit; clients run on the other cores over loopback. Source code does not set CPU affinity or
change host settings. The native load generator reproduces the challenge traffic model.
These short shared-host trials are not official scores. Runs wait for quiet windows;
measurements overlapping another load generator are excluded.

## Batching sweep

At 50,000 virtual users (10 s ramp, 20 s hold), all runs had zero errors. CPU variability
was large enough that the repeated 250 µs baseline is essential:

| Coalescing delay | Hold CPU, µs/request | Hold p95, ms | Hold p99, ms |
|---|---:|---:|---:|
| 250 µs, first control | 39.55 | 1.901 | 4.104 |
| 0 µs | 51.62 | 0.689 | 301.567 |
| 50 µs | 25.88 | 0.459 | 3.163 |
| 100 µs | 33.05 | 1.010 | 3.268 |
| Skip sleep when no requests were served | 50.39 | 11.344 | 310.784 |
| 250 µs, repeated control | 25.42 | 1.149 | 1.754 |

A second comparison used 200,000 virtual users (40 s ramp, 40 s hold):

| Delay | Hold CPU, µs/request | Hold p95, ms | Hold p99, ms | Requests | Errors |
|---|---:|---:|---:|---:|---:|
| 50 µs | 17.56 | 0.717 | 19.168 | 1,490,972 | 0 |
| 250 µs | 18.30 | 1.718 | 25.887 | 1,490,810 | 0 |

An earlier 250 µs control overlapped another load generator and was discarded. These two
valid runs had no detected load-generator overlap. The 50 µs delay is promising for latency,
but its 4% CPU difference in one pair is smaller than observed control variation. Keep
250 µs pending longer repeated comparisons on the checkpoint-thread implementation; do not
claim a regression or a proven win from these short runs.

## Compiler comparison

The `-O3` trial changed only the compiler optimization level from the retained input/cursor
variant, preserving `-march=native` and the other build flags. It passed official and edge
checks. Three interleaved samples per variant produced these thread-CPU medians:

| Workload | `-O2`, ns/request | `-O3`, ns/request |
|---|---:|---:|
| Individual mixed reads | 4,360.72 | 4,371.07 |
| Mixed reads, batches of 32 | 3,102.40 | 3,132.39 |
| Individual post reads | 1,744.54 | 1,793.51 |

The differences do not support switching compiler flags.

## Combined-build validation

The concurrent checkpoint changes were preserved. Validation used the source at commit
`a460db8` (runtime `db.c` SHA-256
`daf4051649102dde2933c655b7b960f0fc71ea365a870f582d2b099b485a87f6`). Runtime, generated C,
Lean sources, and build/start scripts on the existing remote snapshot were compared by
SHA-256 with the working tree at snapshot time. A clean Ubuntu 24.04 installation verified the pinned dependencies, rebuilt
the Lean proofs and generated C, and compiled as a normal user. All 42 official checks
passed. Docker's default seccomp profile initially blocked `io_uring_setup`; the tests were
rerun with `seccomp=unconfined`, as in the earlier passing container run. No host security
setting was changed.

The resulting binary also passed all 42 official checks and the fragmented/pipelined edge
cases under UID 65534, with one server CPU, 2 GiB memory, no swap, and the 65,535 descriptor
limit. These checks exercise the combined checkpoint, input-buffer, and cursor changes.

A final unprivileged smoke test used 300,000 virtual users, a 60 s ramp, 60 s hold,
and 5 s ramp-down (plus the load generator's connection-drain period). It reached
300,000 established connections with no detected competing load generator.

| Combined build | Result |
|---|---:|
| Requests | 3,189,588 |
| Errors | 0 |
| Hold p95 / p99 | 2.643 / 11.919 ms |
| Hold CPU / whole-run CPU | 15.49 / 18.00 µs/request |
| Server-cgroup peak memory | 526.8 MiB |

This is a functional smoke test, not a capacity search or a paired performance comparison
with the old build. Memory is charged server-cgroup memory; copying the seed outside that
cgroup means it is not a measurement of every physical page the server accesses.

A subsequent concurrent edit added `WAL_HIGH` / `db_wal_bound` to `runtime/db.c` while
this smoke test ran. It was preserved, but is outside this validation snapshot. The six
optimization decisions above do not change that separate WAL implementation.
