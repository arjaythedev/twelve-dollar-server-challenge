# C++ + io_uring + SQLite

C++23, liburing 2.15, llhttp 9.4.3, SQLite 3.53.1, yyjson 0.13.0 and OpenSSL 3. CMake pins downloaded sources by SHA-256.

One event-loop thread drives up to 16 io_uring rings with direct descriptors and 64 pending accepts. Rings are added as their file tables fill. Requests use one SQLite connection; a checkpoint worker owns a second connection for PASSIVE checkpoints. Both use unix-excl; external SQLite tools require the server to be stopped. WAL, synchronous=NORMAL and foreign keys are enabled. Reads query SQLite afresh. Up to 16 validated write commands share a transaction with per-write savepoints; responses wait for commit. Batches flush before the loop waits, without a collection delay. Routes are classified and protected requests verify their JWT once after headers; route and identity belong to that request.

Idle connections hold 56 bytes of state. Parser and send storage live in an Exchange during active requests; each ring recycles at most 1,024 Exchanges with up to 8 KiB of retained string capacity each.

## Run

On Ubuntu 24.04, from the repository root:

```sh
sudo bash src/install.sh
bash src/build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash src/start.sh
```

Use a fresh copy of the official seed.

## Development

Python 3.15 and uv run the dev tools.

```sh
uv run --directory src --locked dev.py check
uv run --directory src --locked dev.py check --docker
uv run --directory src --locked dev.py bench --label cpp-350000
uv run --directory src --locked dev.py bench --label profile-5000 --perf all --telemetry profile
uv run --directory src --locked dev.py results
```

Docker checks include transport tests, the 42 official checks and extra HTTP regressions on fresh data. On Windows they also refresh Linux headers for IntelliSense.

Native tests also run directly on Linux with the required io_uring support:

```sh
bash src/build.sh --tests
ctest --test-dir src/.build --output-on-failure
```

Production builds exclude tests. CTest owns native test registration; Docker supplies the Linux test environment on Windows. Diagnostic hooks compile out of normal builds.

`--telemetry off` is the default. `basic` records counters and gauges; `profile` adds timing histograms and bounded five-second history. These modes are independent of perf sampling. The server writes compact JSONL after collection stops. The summary shows missing owners and discarded history.

The server container uses 1 CPU, 2 GiB, the official 65,535 file limit and the profile in src/docker/seccomp.json. Each ring's fixed-file table respects that limit. The profile adds only the three io_uring syscalls to Moby's allowlist. No extra capabilities are needed.

Benchmarks default to 350,000 users, or 5,000 with `--perf`. Both use a 30-second ramp and 60-second hold. Use `--hold 5m --ramp 60s` for confirmation runs.

The native C++ generator follows `bench/load.js` with shared tokens and one connection per user. Its image updates when its sources or build settings change. It supports plain HTTP/IPv4 without redirects or compressed responses. Use `--generator k6` for the pinned Docker reference. Use `--reuse-server` or `--image <tag>` to reuse a server image; native-generator freshness is still checked. Counter readers run outside the server's cgroup.

Perf uses a separate container in the server's PID namespace and samples the hold only, at 997 Hz. `--perf user` records user CPU with DWARF stacks; `kernel` records kernel CPU and `all` includes both. Cgroup counters keep user and system CPU separate. Both generators finish with a 30-second ramp-down and up to 10 seconds of grace.

Runs live in `src/runs/<start-commit>/<label>/`, with a manifest, readable summary and the tested source patch. Detailed logs and profiles stay in the ignored `raw/` directory. `dev.py results` lists the current commit's runs; pass a run path to view it. Use `results <path> --json` for older experiment formats. Paths passed through `uv run --directory src` are relative to src. Perf categories follow whole call paths; leaf CPU shows the sampled function. These views overlap, and unresolved or inlined frames limit precision.
