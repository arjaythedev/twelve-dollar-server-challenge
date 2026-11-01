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

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Local results, not an official score or a measured capacity limit.

The uring rows read CPU counters externally; earlier rows include the in-container counter reader. Their CPU/request values are not directly comparable.

[Data](src/benchmarks/results.json). Full logs stay in `results/<run>/`.

## Native CPU sampling

50,000 users before the B-tree change, 30-second ramp and 60-second capture. User CPU: SQL/rows 68.5%, transport/other 21.9%, response JSON 4.9%, auth 4.6%.

5,443 samples, no loss, 775 unresolved leaves. Hold CPU: 5.915 s user, 9.306 s system.

[Data](src/profiles/native-50000.json). Raw capture: `results/native-current-50000/`.

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
