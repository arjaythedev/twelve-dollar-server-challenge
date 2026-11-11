# TinyJS on Bun

TypeScript, TinyJS (`tinyjs-fullstack` 0.1.2), Bun 1.4.4, and Bun's built-in `bun:sqlite` driver. The entry runs directly on the benchmark VM. TinyJS uses its published production HTTP adapter under Bun.

File-based routes use prepared statements against the challenge's existing indexes. SQLite uses WAL and synchronous NORMAL. Each like runs as a transaction, and token signatures are checked on every authenticated request.

Build with `bash build.sh` and run with `bash start.sh` using the challenge environment variables.

Run the local production smoke test with `bun run smoke.ts` after building.

Version 0.1.2 retains idle HTTP keep-alive connections for 120 seconds, prepares routing precedence once per handler, skips browser output for this API-only application, and minifies the server bundle. No POST retries are added. Framework change and validation: https://github.com/ExpressAI-SWE/TinyJS/pull/4.

On 11 November 2026, the published, lockfile-pinned 0.1.2 package passed all 42 official correctness checks under Ubuntu WSL2 with Bun 1.4.4 and a fresh checksum-verified seed. Its production smoke test also passed concurrent likes, restart persistence and degraded health. The official scripts were run unchanged apart from normalizing shell-script line endings in the disposable Linux copy.

Local shortened probes of the fixed framework reached 2,000 users with 18,538 successful requests, zero errors, p95 15.70 ms and p99 115.34 ms. A higher probe was stopped by the development load generator's memory guard during ramp-up; it does not establish a passing 5,000-user result. The original EOF race was not reproduced in the small local baseline, and neither the official benchmark nor 40,000-user capacity is claimed. An official rerun is required.
