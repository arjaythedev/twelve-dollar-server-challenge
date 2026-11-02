# Benchmarks

2026-11-01 to 2026-11-02. Ryzen 7 7800X3D, Ubuntu 24.04 in Docker Desktop/WSL2. Server: 1 CPU, 2 GiB, no container swap. k6 used the official workload and a fresh database per run, on Windows unless labeled Linux loopback (32 source IPs).

| Runtime | Users | Hold | Requests | Requests/s | p95 | p99 | Max | Errors | CPU/request | Peak RAM |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Python 3.12.13 | 1,000 | 60 s | 10,346 | 79.58 | 1.172 ms | 1.594 ms | 28.03 ms | 0% | 426.84 us | 308.68 MiB |
| Python 3.12.13 | 5,000 | 60 s | 51,795 | 398.42 | 1.125 ms | 3.069 ms | 584.91 ms | 0% | 287.85 us | 327.07 MiB |
| Python 3.12.13 | 5,000 | 5 min | 186,850 | 467.12 | 1.044 ms | 1.459 ms | 35.02 ms | 0% | 273.54 us | 323.30 MiB |
| Python 3.15.2 | 5,000 | 5 min | 186,708 | 466.77 | 1.044 ms | 1.564 ms | 61.09 ms | 0% | 269.54 us | 388.42 MiB |
| Python 3.15.2 + C JSON | 5,000 | 60 s | 51,870 | 399.05 | 1.086 ms | 1.610 ms | 121.65 ms | 0% | 264.36 us | 336.33 MiB |
| C++ 0.1.0 | 5,000 | 60 s | 51,865 | 398.95 | 1.048 ms | 1.568 ms | 189.80 ms | 0% | 132.13 us | 304.56 MiB |
| C++ 0.1.0 (repeat) | 5,000 | 60 s | 51,909 | 399.31 | 1.050 ms | 1.552 ms | 184.86 ms | 0% | 131.93 us | 302.72 MiB |
| C++ 0.1.0 (uring-base, Windows) | 10,000 | 60 s | 103,259 | 794.27 | 1.060 ms | 2.074 ms | 45.99 ms | 0% | 84.23 us | 348.19 MiB |
| C++ 0.1.0 (uring-coop, Windows) | 10,000 | 60 s | 103,820 | 798.59 | 1.050 ms | 1.605 ms | 33.10 ms | 0% | 82.84 us | 343.96 MiB |
| C++ 0.1.0 (uring-batch, Windows) | 10,000 | 60 s | 103,745 | 798.01 | 2.609 ms | 5.363 ms | 34.67 ms | 0% | 81.92 us | 340.11 MiB |
| C++ 0.1.0 (uring-base, Linux loopback) | 50,000 | 60 s | 518,222 | 3,984.46 | 0.584 ms | 16.421 ms | 55.84 ms | 0% | 54.58 us | 681.55 MiB |
| C++ 0.1.0 (uring-batch, Linux loopback) | 50,000 | 60 s | 457,791 | 3,519.70 | 4,804.318 ms | 5,586.012 ms | 54,987.35 ms | 1.94% | 47.02 us | 672.58 MiB |
| C++ 0.1.0 (adaptive, Linux loopback) | 50,000 | 60 s | 518,877 | 3,989.27 | 1.478 ms | 18.388 ms | 55.68 ms | 0% | 46.25 us | 692.12 MiB |
| C++ 0.1.0 (B-tree feed, Linux loopback) | 50,000 | 60 s | 518,213 | 3,984.43 | 1.474 ms | 20.657 ms | 108.56 ms | 0% | 45.00 us | 687.91 MiB |
| C++ 0.1.0 (2-ms wait, Linux loopback) | 50,000 | 60 s | 518,114 | 3,983.08 | 2.753 ms | 21.918 ms | 263.26 ms | 0% | 44.06 us | 684.10 MiB |
| C++ 0.1.0 (deferred taskwork, Linux loopback) | 50,000 | 60 s | 518,430 | 3,986.05 | 1.632 ms | 21.604 ms | 132.07 ms | 0% | 46.12 us | 699.32 MiB |
| C++ 0.1.0 (B-tree feed repeat, Linux loopback) | 50,000 | 60 s | 518,190 | 3,984.10 | 1.536 ms | 21.952 ms | 118.30 ms | 0% | 45.70 us | 682.38 MiB |
| C++ 0.1.0 (range/local + 2-ms wait, Linux loopback) | 50,000 | 60 s | 518,437 | 3,986.13 | 2.409 ms | 18.236 ms | 69.87 ms | 0% | 38.86 us | 692.95 MiB |
| C++ 0.1.0 (range/local + 2-ms repeat, Linux loopback) | 50,000 | 60 s | 517,392 | 3,978.10 | 2.587 ms | 21.344 ms | 104.77 ms | 0% | 40.02 us | 686.48 MiB |
| C++ 0.1.0 (shared receive pool, Linux loopback) | 50,000 | 60 s | 517,485 | 3,978.64 | 2.448 ms | 19.221 ms | 106.41 ms | 0% | 38.78 us | 466.52 MiB |
| C++ 0.1.0 (scatter-gather sends, Linux loopback) | 50,000 | 60 s | 518,431 | 3,986.25 | 2.403 ms | 20.840 ms | 114.77 ms | 0% | 37.27 us | 422.15 MiB |
| C++ 0.1.0 (32 CQEs / 4-ms wait, Linux loopback) | 50,000 | 60 s | 518,522 | 3,986.87 | 4.646 ms | 22.405 ms | 112.91 ms | 0% | 33.52 us | 422.06 MiB |
| C++ 0.1.0 (64 CQEs / 8-ms wait, Linux loopback) | 50,000 | 60 s | 517,863 | 3,981.45 | 8.645 ms | 22.215 ms | 97.42 ms | 0% | 33.64 us | 422.19 MiB |
| C++ 0.1.0 (direct-parse baseline, Linux loopback) | 50,000 | 60 s | 517,785 | 3,980.96 | 4.652 ms | 21.708 ms | 106.82 ms | 0% | 34.27 us | 421.78 MiB |
| C++ 0.1.0 (direct receive parsing, Linux loopback) | 50,000 | 60 s | 518,140 | 3,983.96 | 4.661 ms | 21.401 ms | 111.44 ms | 0% | 34.73 us | 405.88 MiB |
| C++ 0.1.0 (low-hanging baseline A, Linux loopback) | 25,000 | 60 s | 259,411 | 1,995.17 | 4.643 ms | 15.593 ms | 99.42 ms | 0% | 39.79 us | 341.71 MiB |
| C++ 0.1.0 (low-hanging A, Linux loopback) | 25,000 | 60 s | 259,483 | 1,995.82 | 4.632 ms | 15.321 ms | 91.17 ms | 0% | 39.34 us | 341.66 MiB |
| C++ 0.1.0 (low-hanging baseline B, Linux loopback) | 25,000 | 60 s | 259,468 | 1,995.52 | 4.645 ms | 14.754 ms | 99.35 ms | 0% | 39.69 us | 341.40 MiB |
| C++ 0.1.0 (low-hanging B, Linux loopback) | 25,000 | 60 s | 259,229 | 1,993.62 | 4.640 ms | 15.178 ms | 95.01 ms | 0% | 39.48 us | 341.59 MiB |
| C++ 0.1.0 (direct-descriptor baseline A, Linux loopback) | 25,000 | 60 s | 259,288 | 1,993.44 | 4.634 ms | 14.389 ms | 68.82 ms | 0% | 40.46 us | 343.40 MiB |
| C++ 0.1.0 (direct descriptors A, Linux loopback) | 25,000 | 60 s | 259,013 | 1,991.54 | 4.640 ms | 14.730 ms | 65.56 ms | 0% | 40.49 us | 342.45 MiB |
| C++ 0.1.0 (direct-descriptor baseline B, Linux loopback) | 25,000 | 60 s | 259,159 | 1,992.99 | 4.634 ms | 14.107 ms | 92.20 ms | 0% | 40.56 us | 341.35 MiB |
| C++ 0.1.0 (direct descriptors B, Linux loopback) | 25,000 | 60 s | 259,214 | 1,993.26 | 4.639 ms | 14.077 ms | 92.99 ms | 0% | 40.49 us | 342.55 MiB |
| C++ 0.1.0 (refactor baseline, Linux loopback) | 50,000 | 60 s | 519,120 | 3,990.66 | 4.681 ms | 21.253 ms | 125.79 ms | 0% | 34.47 us | 431.94 MiB |
| C++ 0.1.0 (compact transport, Linux loopback) | 50,000 | 60 s | 518,181 | 3,983.89 | 4.612 ms | 20.346 ms | 117.51 ms | 0% | 33.59 us | 407.12 MiB |
| C++ 0.1.0 (native client, Linux loopback) | 100,000 | 60 s | 1,036,376 | 7,972.07 | 11.246 ms | 26.269 ms | 120.47 ms | 0% | 32.54 us | 653.45 MiB |
| C++ 0.1.0 (hybrid send, Linux loopback) | 100,000 | 60 s | 1,036,840 | 7,975.70 | 12.957 ms | 33.165 ms | 125.39 ms | 0% | 32.52 us | 647.02 MiB |
| C++ 0.1.0 (cold Exchange, Linux loopback) | 100,000 | 60 s | 1,036,362 | 7,972.03 | 11.366 ms | 27.424 ms | 455.02 ms | 0% | 31.05 us | 528.11 MiB |
| C++ 0.1.0 (native client, failed) | 300,000 | 5 min | 10,284,638 | 25,711.54 | 5,316.581 ms | 19,524.643 ms | 60,001.25 ms | 0.0066% | 25.80 us | 1,391.69 MiB |
| C++ 0.1.0 (bounded handler queue) | 100,000 | 60 s | 1,037,202 | 7,978.44 | 12.390 ms | 32.713 ms | 201.77 ms | 0% | 31.32 us | 525.76 MiB |
| C++ 0.1.0 (checkpoint worker) | 100,000 | 60 s | 1,037,228 | 7,978.67 | 3.690 ms | 4.711 ms | 34.51 ms | 0% | 32.70 us | 535.91 MiB |
| C++ 0.1.0 (checkpoint worker, failed) | 300,000 | 5 min | 11,039,291 | 27,598.20 | 75.035 ms | 3,979.977 ms | 33,316.56 ms | 0% | 27.86 us | 1,857.30 MiB |
| C++ 0.1.0 (300k profile) | 300,000 | 60 s | 3,586,049 | 22,412.72 | 5.080 ms | 8.026 ms | 94.87 ms | 0% | 28.42 us | 1,522.39 MiB |
| C++ 0.1.0 (idle poll, reverted) | 300,000 | 60 s | 3,542,624 | 22,141.34 | 72.329 ms | 4,170.837 ms | 7,246.31 ms | 0% | 30.26 us | 1,993.39 MiB |
| C++ 0.1.0 (feed batch) | 300,000 | 60 s | 3,586,146 | 22,413.30 | 5.067 ms | 8.487 ms | 139.13 ms | 0% | 28.32 us | 1,361.25 MiB |
| C++ 0.1.0 (feed batch) | 300,000 | 5 min | 11,148,015 | 27,869.99 | 16.217 ms | 749.363 ms | 23,529.85 ms | 0% | 26.69 us | 1,618.10 MiB |
| C++ 0.1.0 (diagnostics) | 300,000 | 5 min | 10,846,881 | 27,117.20 | 390.067 ms | 9,904.171 ms | 38,276.73 ms | 0% | 26.47 us | 2,354.75 MiB |
| C++ 0.1.0 (feed batch control, failed) | 300,000 | 5 min | 11,139,199 | 27,847.99 | 134.529 ms | 1,702.041 ms | 14,729.54 ms | 0% | 26.45 us | 1,982.40 MiB |
| C++ 0.1.0 (group commit) | 300,000 | 60 s | 3,581,785 | 22,386.04 | 28.255 ms | 371.544 ms | 4,785.60 ms | 0% | 25.95 us | 1,097.05 MiB |
| C++ 0.1.0 (group commit) | 300,000 | 5 min | 11,200,015 | 28,000.03 | 5.360 ms | 82.080 ms | 3,954.01 ms | 0% | 24.36 us | 1,247.85 MiB |

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Local results, not an official score or a measured capacity limit.

300k feed batch, five minutes: hold-only p99 1,172.77 ms; aggregate p99 749.36 ms.

300k group commit, five minutes, no internal telemetry: hold-only p99 97.99 ms; maximum 3,954.01 ms. The short run had worse p99 than the earlier short feed profile.

Normal repeat: same server/client/seed/workload hashes, different RNG seed; p99 rose from 749.36 to 1,702.04 ms. No memory-limit or reclaim events.

Diagnostics: foreground COMMIT maxima 2,526.89 and 1,426.01 ms; full CQs and CPU saturation afterward. Client timer p99 about 1.1 ms, epoll p99 below 0.25 ms. Memory limit reached during hold, no OOM. [Data](src/profiles/stalls-300000.json).

Low-hanging and direct-descriptor pairs ran one baseline and one candidate concurrently. Direct-descriptor pairs used the official 65,535 file limit.

Transport check: 70,000 open connections with `RLIMIT_NOFILE=65535`, two rings, 1 CPU and 2 GiB. Slot reuse and shutdown passed.

The uring rows read CPU counters externally; earlier rows include the in-container counter reader. Their CPU/request values are not directly comparable.

[Data](src/benchmarks/results.json). [300k CPU profile](src/profiles/peak-cpu-300000.json). Full logs stay in `results/<run>/`.

Feed batching: native median 10.58 to 9.06 us over five alternating pairs; after cleanup, 10.42 to 9.05 us over three pairs. Outputs matched. [Data](src/benchmarks/feed-batch.json).

Group commit, 10,000 valid writes, two reversed pairs: b=1 to b=16 reduced median CPU/write from 63.85 to 51.88 us, WAL frames from 53,424 to 41,488, commits from 10,000 to 625. Outputs/state matched; inline checkpoints, no HTTP timing. b=64 saved more CPU in the mixed test but showed unstable wall time. [Data](src/benchmarks/group-commit.json).

`SQLITE_DEFAULT_MEMSTATUS=0`, seven alternating CPU comparisons: feed paired median 1.0% slower, post 0.9% faster. All JSON hashes matched.

Release LTO, seven alternating CPU comparisons: feed medians 10.358 to 10.227 us, post 4.086 to 4.169 us; executable text 10.1% smaller. All JSON hashes matched.

`-mtune=cascadelake`, 7 pairs: feed 10.218 to 10.231 us; post 4.088 to 4.041 us.

GCC13 to GCC16 (C++23/GNU17), 7 pairs: feed 10.147 to 10.279 us; post 4.081 to 4.127 us. GCC16 needed a SQLite warning exception.

SQLite page cache, 2 to 16 MiB, five alternating pairs: random-post CPU median change -12.6%, misses -37.3%, feed unchanged. JSON hashes matched. Mixed 10k profile: 51.85 us/request, no errors, no clear overall CPU change. [Data](src/profiles/cache16-10000.json).

Load generator, 10k users / 60 s: sampled client cgroup peak 2,775.08 MiB (k6) versus 52.63 MiB (native); 795.95 versus 797.57 requests/s, no errors. Native process peak RSS: 24.05 MiB. [Data](src/profiles/cpp-load-10000.json).

Accept depth 1 to 64, 512-connection bursts behind a 20-ms handler gate, file limit 256: actual accepts during the gate 1 to 64; after-release p99 10.223 to 6.106 ms and 9.011 to 6.281 ms. All responses passed. [Data](src/benchmarks/accept-depth.json).

Parser reuse, 100k lifecycle cycles, four alternating samples: JWT/body CPU median 29.098 to 20.631 ms, allocations 250,009 to 16. With 25% oversized bodies: 30.288 to 27.834 ms, allocations 250,009 to 275,006 as oversized Exchanges are discarded. Checksums matched. Reset code: 1,335 to 267 bytes.

## Native CPU sampling

50,000 users before the B-tree change, 30-second ramp and 60-second capture. User CPU: SQL/rows 68.5%, transport/other 21.9%, response JSON 4.9%, auth 4.6%.

5,443 samples, no loss, 775 unresolved leaves. Hold CPU: 5.915 s user, 9.306 s system.

[Data](src/profiles/native-50000.json). Raw capture: `results/native-current-50000/`.

Kernel capture after the B-tree change, same load and window: network/socket paths 57.7%, io_uring 19.1%, files/locks/page cache 15.6%. Attribution follows stack paths. By syscall, including nested work: io_uring_enter 76.1%, fcntl/unixShmLock 10.6%, pwrite64 4.5%, fsync 2.2%, pread64 1.0%. 8,017 samples, no loss, two unresolved leaves. Hold CPU: 4.963 s user, 8.784 s system. No request errors. Loopback includes peer/ACK work in server context; WAL versus checkpoint writes remain unresolved.

[Data](src/profiles/kernel-50000.json). Raw capture: `results/kernel-btree-50000/`.

Current combined capture, same load and window: network 38.2%, SQL/rows 21.8%, io_uring 12.8%, files/locks/page cache 10.9%, JSON 2.1%, auth 2.0% of sampled CPU. 10,549 samples, no loss, 435 unresolved leaves. No request errors. [Data](src/profiles/combined-50000.json).

Compact transport, 10,000 users, 30-second ramp and 60-second capture: network 37.1%, SQL/rows 20.4%, io_uring 17.9%, files/locks/page cache 9.7%. Leaf CPU: interrupt unlock 10.8%, `finish_task_switch` 8.4%. 2,657 samples, no loss, 101 unresolved leaves, no request errors. [Data](src/profiles/compact-10000.json).

## Request-path sampling

Python 3.15.2, 5,000 users, five-minute hold. External CPU/wall samplers at 1 kHz on Granian's Python request thread. Unchanged API. The load run passed: 186,776 requests, p95 1.056 ms, p99 1.616 ms, no errors.

| Within captured handler stacks | CPU samples | Wall samples |
| --- | ---: | ---: |
| SQL + database wrapper | 49.5% | 52.7% |
| Row materialization | 0.1% | 0.1% |
| Response JSON | 31.2% | 24.6% |
| Auth, including JWT JSON | 3.2% | 2.6% |
| Routing, response handoff and other | 16.0% | 20.0% |

5,842 handler CPU samples and 8,521 handler wall samples. Another 59.8% of valid CPU samples were outside the handler, mainly asyncio/event-loop work. Native HTTP threads and suspended async waits are outside this profile's scope.

These are sampling estimates, not exact function timings. Stack-read failures: CPU 2.58%, wall 2.44%. Body-JSON parsing and GC had no captured samples; that does not establish zero cost.

Raw samples and flamegraphs: `results/request-profile-5000/`.
