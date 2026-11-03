# Results: CachyOS x86_64 (Ryzen 9 9955HX), droplet-limits containers
# (1 CPU pinned, 2 GiB, no swap, nofile=65535), loopback, k6 v2.3.0 on host cores.
# Absolute numbers won't match the droplet vCPU; rankings and limiters should.

## Correctness (official test/test.sh, fresh DB)
| Submission | Result |
|---|---|
| ours (c-epoll) | 42/42 |
| #26 C/io_uring (champion) | 42/42 |
| #5 C/fio-stl | 42/42 |
| #42 C/epoll | 42/42 |
| #15 C++/uWebSockets v2 | 42/42 |
| #19 Rust/epoll | 42/42 |
| #25 Zig/uSockets | 42/42 |

## Saturation (mixed 100:100:15:2, 64 conns, 2x20s, fresh DB each; medians, this box)
| Submission | req/s | Notes |
|---|---|---|
| ours | ~112k | single-thread epoll, PGO (final binary) |
| #19 Rust/epoll | ~107k | tie (2 sweeps) |
| #26 C/io_uring | ~94k | 8 workers, connection-optimized (90-95k) |
| #15 C++/uWS v2 | ~83k | |
| #42 C/epoll | ~81k | |
| #25 Zig/uSockets | ~80k | |
| #5 C/fio-stl | ~67k | (one early 89k reading not reproduced; 4 runs at 65-69k) |
(k6 itself burns ~4.5 cores parsing 13 KB feed bodies at these rates; all face the same k6.
At >100k rps k6 reports ~8% failed requests with zero timeouts, zero warnings and
p99 ~1.3 ms: a tight-loop accounting quirk, not server errors. Evidence: an independent
8-thread x 500 sequential-request validator checks status + JSON + post count on every
response: 4000/4000 exact; and every official-protocol run (5M+ requests) reports 0.00% failed.)
Early readings taken under background load were discarded; table uses contention-free runs.)

## Capacity (official bench/load.js; multi-IP k6: 28k ephemeral ports per dst)
| Submission | Users | Hold | p95 | p99 | Errors | Verdict |
|---|---|---|---|---|---|---|
| ours | 70,000 | 2 min | ~220µs | ~8ms | 0 | PASS |
| ours | 85,000 | 2 min | ~400µs | ~8.5ms | 0 / 1.56M | **PASS** (final binary; k6 RAM is the next wall) |
| ours | 110,000 | 2 min | ~360µs | ~533µs | 4 / 1.78M | **PASS** (final binary) |
| ours | 110,000 | 5 min (official-shaped) | ~370µs | ~531µs | 9 / 4.45M | **PASS** (final binary) |
| ours | 120,000 | 2 min | ~510ms | ~589ms | 1 / 1.87M | marginal FAIL (p95 over 500ms) |
| ours | 135,000 | 2 min | ~606ms | ~844ms | 12 / 1.91M | FAIL (p95) |
| ours | 150,000 | 2 min | ~772ms | ~940ms | 313 / 1.9M | FAIL (p95) |
| ours | 70,000 | 5 min (official) | ~113µs | ~5.6ms | 0 / 2.61M | **PASS** (final binary) |
| ours | 40,000 | 2 min | ~110µs | ~4ms | 0 | PASS |
| ours | 10,000 | 2 min | ~405µs | ~650µs | 0 | PASS |
| #26 champion | 70,000 | 5 min (official) | ~5ms | ~10.6ms | 0 / 2.6M | **PASS** (~330 MB container) |
| #5 C/fio | 70,000 | 2 min | n/a | n/a | 0 / 1.28M | **PASS** |

## Durability / fault behavior
| Test | Result |
|---|---|
| SIGKILL mid-load + restart, 300 acknowledged creates | 300/300 read back byte-correct |
| FK violations (ghost uid 999999) on create/like | 500, never phantom 201 |
| Mid-batch rollback poisons batch (flag + convert) | implemented, reviewed (fault-injection via file cap pending) |
| Double-start (second bind) | fails loudly (no SO_REUSEPORT); never splits traffic |

## A/B decisions (kept vs rejected, measured on this box)
| Option | Result |
|---|---|
| Feed 3-range-scan (champion's technique) | REJECTED: 13.4µs vs reference 8.9µs at SQLite level |
| mmap 1 GiB cap (vs 256 MiB) | KEPT: +3% saturation, fits 2 GB (virtual) |
| INSERT + SELECT created_at (vs RETURNING) | KEPT by construction (#19's rationale: no ephemeral table) |
| SO_REUSEPORT | REMOVED: second instance must fail loudly, never split traffic |
