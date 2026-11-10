# TinyJS on Bun

TypeScript, TinyJS (`tinyjs-fullstack` 0.1.1), Bun 1.4.4, and Bun's built-in `bun:sqlite` driver. The entry runs directly on the benchmark VM. TinyJS uses its published production HTTP adapter under Bun.

File-based routes use prepared statements against the challenge's existing indexes. SQLite uses WAL and synchronous NORMAL. Each like runs as a transaction, and token signatures are checked on every authenticated request.

Build with `bash build.sh` and run with `bash start.sh` using the challenge environment variables.

Run the local production smoke test with `bun run smoke.ts` after building.
