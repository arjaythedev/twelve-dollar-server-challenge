# C++ + io_uring + SQLite

C++23, liburing 2.15, llhttp 9.4.3, SQLite 3.53.1, yyjson 0.13.0 and OpenSSL 3. CMake pins downloaded sources by SHA-256.

One io_uring event loop and one SQLite connection, with WAL, synchronous=NORMAL and foreign keys enabled. Reads query SQLite afresh. Writes commit before their response. Each protected request verifies its JWT after the headers; its identity lasts for that request only.

## Run

On Ubuntu 24.04, from the repository root:

```sh
sudo bash src/install.sh
bash src/build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash src/start.sh
```

Use a fresh copy of the official seed.

## Development

Python 3.15 and uv are only used for tests and benchmarks.

```sh
uv run --directory src --locked dev.py check
uv run --directory src --locked dev.py check --docker
uv run --directory src --locked dev.py bench --label cpp-5000 --k6 /path/to/k6
uv run --directory src --locked dev.py results
```

Docker checks include transport tests, the 42 official checks and extra HTTP regressions on fresh data. On Windows they also refresh Linux headers for IntelliSense.

The container uses 1 CPU, 2 GiB and the profile in src/docker/seccomp.json. It adds only the three io_uring syscalls to Moby's allowlist. No extra capabilities are needed.

Benchmarks default to a 30-second ramp and 60-second hold. Use `--hold 5m --ramp 60s` for confirmation runs. Native profiling is pending. Previous benchmark results remain available.
