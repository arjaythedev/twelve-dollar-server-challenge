# TinyJS on Bun

TypeScript, TinyJS (`tinyjs-fullstack` 0.1.2), Bun 1.4.4, and Bun's built-in `bun:sqlite` driver. The entry runs directly on the benchmark VM. `transport.ts` connects Bun's native HTTP server to TinyJS's built application handler. Routing and endpoint handling stay in TinyJS; the Node HTTP conversions are removed. This entry serves this API-only build, not TinyJS browser assets.

File-based routes use prepared statements against the challenge's existing indexes. SQLite uses WAL and synchronous NORMAL. Each like runs as a transaction, and token signatures are checked on every authenticated request.

Build with `bash build.sh` and run with `bash start.sh` using the challenge environment variables.

Run the local production smoke test with `bun run smoke.ts` after building.

Run `bun run smoke.ts transport.ts` to check the native transport. `start.sh` defaults to native Bun; `TINYJS_TRANSPORT=node bash start.sh` selects the original published Node HTTP adapter running under Bun for comparison. Both retain a 120-second idle timeout. The framework's Node transport remains available; the submission itself requires Bun for SQLite.

On 12 November 2026, both transports passed all 42 official correctness checks on separate fresh copies of a checksum-verified seed under Ubuntu WSL2 and Bun 1.4.4. Both also passed the production smoke test, including HEAD body suppression and method rejection. Authentication, SQL, indexes and durability settings are unchanged; no response, result or JWT verification cache was added.

For a local read comparison, run `SQLITE_PATH=/path/to/disposable-seeded.db bun compare.ts` after building on Linux. The probe warms each server with 200 reads, then uses 16 concurrent clients for five seconds in Node/Bun/Bun/Node order. It reads `/feed` and `/posts/1`, `/posts/2`, `/posts/3`, consumes every response and asserts HTTP 200. Server CPU comes from `/proc`, excluding the load generator; CPU time has clock-tick resolution. It does not run the official mixed workload.

Local results (12 November 2026):

| Transport / round | Requests/s | Server CPU microseconds/request | p95 ms | p99 ms | Peak RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Node / 1 | 11,224 | 95.84 | 3.124 | 4.811 | 82,952 |
| Bun / 2 | 28,070 | 33.41 | 1.131 | 2.229 | 51,452 |
| Bun / 3 | 30,789 | 30.40 | 0.943 | 2.034 | 51,580 |
| Node / 4 | 10,456 | 102.31 | 3.066 | 4.879 | 83,568 |

These short WSL2 probes share the development machine with the client, are not CPU-pinned or constrained to the challenge VM's resources, and establish no user-capacity score. They support testing native transport on the official server. No CPU attribution profile, router optimization or SQLite tuning is included in this experiment.

Version 0.1.2 retains idle HTTP keep-alive connections for 120 seconds, prepares routing precedence once per handler, skips browser output for this API-only application, and minifies the server bundle. No POST retries are added. Framework change and validation: https://github.com/ExpressAI-SWE/TinyJS/pull/4.

On 11 November 2026, the published, lockfile-pinned 0.1.2 package passed all 42 official correctness checks under Ubuntu WSL2 with Bun 1.4.4 and a fresh checksum-verified seed. Its production smoke test also passed concurrent likes, restart persistence and degraded health. The official scripts were run unchanged apart from normalizing shell-script line endings in the disposable Linux copy.

Local shortened probes of the fixed framework reached 2,000 users with 18,538 successful requests, zero errors, p95 15.70 ms and p99 115.34 ms. A higher probe was stopped by the development load generator's memory guard during ramp-up; it does not establish a passing 5,000-user result. The original EOF race was not reproduced in the small local baseline, and neither the official benchmark nor 40,000-user capacity is claimed. An official rerun is required.
