# C++ + io_uring + SQLite

C++23, liburing 2.15, llhttp 9.4.3, SQLite 3.53.1, yyjson 0.13.0 and OpenSSL 3. CMake pins downloaded sources by SHA-256.

One event-loop thread drives up to 16 io_uring rings with direct descriptors and one SQLite connection. Rings are added as their file tables fill. WAL, synchronous=NORMAL and foreign keys are enabled. Reads query SQLite afresh. Writes commit before their response. Each protected request verifies its JWT after the headers; its identity lasts for that request only.

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
uv run --directory src --locked dev.py bench --label cpp-50000 --vus 50000
uv run --directory src --locked dev.py profile --label native-10000 --vus 10000 --save profiles/native-10000.json
uv run --directory src --locked dev.py results
```

Docker checks include transport tests, the 42 official checks and extra HTTP regressions on fresh data. On Windows they also refresh Linux headers for IntelliSense.

The server container uses 1 CPU, 2 GiB, the official 65,535 file limit and the profile in src/docker/seccomp.json. Each ring's fixed-file table respects that limit. The profile adds only the three io_uring syscalls to Moby's allowlist. No extra capabilities are needed.

Benchmarks and profiles default to a 30-second ramp and 60-second hold. Use `--hold 5m --ramp 60s` for confirmation runs.

Both use the unchanged `bench/load.js`; k6 reports latency, throughput and errors. The wrapper manages Docker, resource counters and saved results. Use `--no-build` to reuse existing images. Counter readers run outside the server's cgroup.

Profiles use a separate perf container with CAP_PERFMON, the same UID and the server's PID namespace. Sampling covers the hold only, using `cpu-clock:u` at 997 Hz with DWARF stacks and the unchanged release binary. Kernel work and io_uring workers are outside the stack profile; cgroup counters record user and system CPU separately. k6 still runs its final 30-second ramp-down.

Raw profiles stay in `results/<label>/`. `--save` keeps a compact summary; `dev.py results <profile.json>` displays it. Paths passed through `uv run --directory src` are relative to src. Stage attribution uses stack names; inlining and unresolved frames limit precision.
