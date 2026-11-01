# Benchmarks

2026-11-01. Python 3.12.13, Ryzen 7 7800X3D, Ubuntu 24.04 in Docker Desktop/WSL2. Server: 1 CPU, 2 GiB, no container swap. k6 ran on Windows with the official workload and a fresh database per run.

| Users | Hold | Requests | Requests/s | p95 | p99 | Errors | CPU/request | Peak RAM |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 60 s | 10,346 | 79.58 | 1.172 ms | 1.594 ms | 0% | 426.84 us | 308.68 MiB |
| 5,000 | 60 s | 51,795 | 398.42 | 1.125 ms | 3.069 ms | 0% | 287.85 us | 327.07 MiB |
| 5,000 | 5 min | 186,850 | 467.12 | 1.044 ms | 1.459 ms | 0% | 273.54 us | 323.30 MiB |

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Maximum latency in the short 5,000-user run: 584.91 ms. Local results, not an official score or a measured capacity limit.

[Data](src/benchmarks/results.json). Full logs stay in `results/<run>/`.
