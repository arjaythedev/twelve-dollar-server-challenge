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

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Local results, not an official score or a measured capacity limit.

Low-hanging pairs ran one baseline and one candidate concurrently.

The uring rows read CPU counters externally; earlier rows include the in-container counter reader. Their CPU/request values are not directly comparable.

[Data](src/benchmarks/results.json). Full logs stay in `results/<run>/`.

`SQLITE_DEFAULT_MEMSTATUS=0`, seven alternating CPU comparisons: feed paired median 1.0% slower, post 0.9% faster. All JSON hashes matched.

Release LTO, seven alternating CPU comparisons: feed medians 10.358 to 10.227 us, post 4.086 to 4.169 us; executable text 10.1% smaller. All JSON hashes matched.

`-mtune=cascadelake`, 7 pairs: feed 10.218 to 10.231 us; post 4.088 to 4.041 us.

GCC13 to GCC16 (C++23/GNU17), 7 pairs: feed 10.147 to 10.279 us; post 4.081 to 4.127 us. GCC16 needed a SQLite warning exception.

## Native CPU sampling

50,000 users before the B-tree change, 30-second ramp and 60-second capture. User CPU: SQL/rows 68.5%, transport/other 21.9%, response JSON 4.9%, auth 4.6%.

5,443 samples, no loss, 775 unresolved leaves. Hold CPU: 5.915 s user, 9.306 s system.

[Data](src/profiles/native-50000.json). Raw capture: `results/native-current-50000/`.

Kernel capture after the B-tree change, same load and window: network/socket paths 57.7%, io_uring 19.1%, files/locks/page cache 15.6%. Attribution follows stack paths. By syscall, including nested work: io_uring_enter 76.1%, fcntl/unixShmLock 10.6%, pwrite64 4.5%, fsync 2.2%, pread64 1.0%. 8,017 samples, no loss, two unresolved leaves. Hold CPU: 4.963 s user, 8.784 s system. No request errors. Loopback includes peer/ACK work in server context; WAL versus checkpoint writes remain unresolved.

[Data](src/profiles/kernel-50000.json). Raw capture: `results/kernel-btree-50000/`.

Current combined capture, same load and window: network 38.2%, SQL/rows 21.8%, io_uring 12.8%, files/locks/page cache 10.9%, JSON 2.1%, auth 2.0% of sampled CPU. 10,549 samples, no loss, 435 unresolved leaves. No request errors. [Data](src/profiles/combined-50000.json).

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
