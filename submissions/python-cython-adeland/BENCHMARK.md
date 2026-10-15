# Recorded benchmark results

## Build and configuration

Direct HTTP/1.1 using Python/Cython and llhttp, without Nginx or ASGI. Pinned build
dependencies: Cython 3.1.6, setuptools 80.9.0, llhttp 9.3.0, yyjson 0.12.0. Python,
SQLite, and OpenSSL use Ubuntu 24.04 packages. Parser archives are SHA256-verified.

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/fresh/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

The service runs as a normal user with `LimitNOFILE=65535` and
`CAP_NET_BIND_SERVICE`. It reads only the four environment variables above and
runs in the foreground. Use port 3000 for unprivileged local checks.


## Original descriptor limit on one GCE vCPU, 2026-10-14

Server: `custom-1-2048`, one non-shared guest vCPU on Intel Haswell, 2 GiB allocated
RAM (1,960 MiB usable), Ubuntu 24.04 x86_64, no swap. The server served
HTTP/1.1 directly on port 80 as user `ubuntu`, with `CAP_NET_BIND_SERVICE`.
The running process's soft **and hard** descriptor limits were verified as 65,535.
Our connection table used its default 65,536 slots.

Each confirmation started with a fresh process/database and a 1,000-user,
two-minute warm-up. The unchanged `bench/load.js` then ran a 60-second ramp,
five-minute hold, and 30-second down-ramp with 10-second grace. Percentiles and
rates include the ramps. This was a confirmation near the descriptor ceiling,
not another doubling search from 2,500 users.

| Users | Result | p95 | p99 | HTTP errors | Average HTTP RPS | Sampled process HWM |
|---|---|---:|---:|---:|---:|---:|
| 65,500 | pass | 82.14 ms | 242.84 ms | 0% | 6,090.93 | 114.38 MiB |

All 2,437,538 requests passed their status checks. The server passed all 42 official
checks and 44 additional checks before load. The trial had no OOM
event or new NIC packet drop/error.

The server reached 65,508 open descriptors, leaving 27 under the imposed limit. Nominal
plateau process CPU averaged 38.93%. Whole-guest busy CPU was 46.23%, with I/O wait
separately at 6.74%. Minimum sampled available RAM was 1,263.73 MiB. This establishes
a passing load near the descriptor ceiling; the server's processing maximum
was not measured on this VM.

The separate eight-vCPU, 96-GiB generator used one k6 process. Central-plateau CPU
averaged 3.29 of eight vCPUs; peak RSS was 20.92 GiB. The server kernel was unchanged
and the server process was not pinned.
The GCP CPU, same-zone private network, 24-GB pd-balanced disk, and suppressed
maintenance timers differ from the official DigitalOcean environment.

Raw results, frozen sources/binaries, hashes, runtime-contract proofs, and telemetry:
`results/gcp-onecpu/original-limits/`. Its `verified-summary.json` checks source
hashes, request/status counts, runtime limits, and telemetry coverage. The excluded
180,000-user experiments do not establish a processing maximum; the final sharded
experiment was stopped at the user's request.
