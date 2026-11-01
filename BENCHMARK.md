# Benchmarks

2026-11-01. Ryzen 7 7800X3D, Ubuntu 24.04 in Docker Desktop/WSL2. Server: 1 CPU, 2 GiB, no container swap. k6 ran on Windows with the official workload and a fresh database per run.

| Runtime | Users | Hold | Requests | Requests/s | p95 | p99 | Max | Errors | CPU/request | Peak RAM |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Python 3.12.13 | 1,000 | 60 s | 10,346 | 79.58 | 1.172 ms | 1.594 ms | 28.03 ms | 0% | 426.84 us | 308.68 MiB |
| Python 3.12.13 | 5,000 | 60 s | 51,795 | 398.42 | 1.125 ms | 3.069 ms | 584.91 ms | 0% | 287.85 us | 327.07 MiB |
| Python 3.12.13 | 5,000 | 5 min | 186,850 | 467.12 | 1.044 ms | 1.459 ms | 35.02 ms | 0% | 273.54 us | 323.30 MiB |
| Python 3.15.2 | 5,000 | 5 min | 186,708 | 466.77 | 1.044 ms | 1.564 ms | 61.09 ms | 0% | 269.54 us | 388.42 MiB |
| Python 3.15.2 + C JSON | 5,000 | 60 s | 51,870 | 399.05 | 1.086 ms | 1.610 ms | 121.65 ms | 0% | 264.36 us | 336.33 MiB |
| C++ 0.1.0 | 5,000 | 60 s | 51,865 | 398.95 | 1.048 ms | 1.568 ms | 189.80 ms | 0% | 132.13 us | 304.56 MiB |
| C++ 0.1.0 (repeat) | 5,000 | 60 s | 51,909 | 399.31 | 1.050 ms | 1.552 ms | 184.86 ms | 0% | 131.93 us | 302.72 MiB |

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Local results, not an official score or a measured capacity limit.

[Data](src/benchmarks/results.json). Full logs stay in `results/<run>/`.

## Native CPU sampling

5,000 users, 60-second hold, `cpu-clock:u` at 997 Hz. SQL/row assembly: 63.4%; transport/other: 27.9%; JSON output: 4.7%; auth: 3.9%. No request-JSON samples were captured.

1,369 samples, no loss records, 167 unresolved leaf symbols and one stack without main. These shares cover sampled server user CPU. Kernel work and the benchmark sampler are excluded. The container counters include both: 43.5% user CPU, 56.5% system CPU.

[Data](src/profiles/native-5000.json). Raw capture: `results/cpp-profile-5000/`.

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
