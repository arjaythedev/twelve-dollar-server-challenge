# TinyJS on Bun

TypeScript, TinyJS (`tinyjs-fullstack` 0.1.0), Bun 1.4.4, and Bun's built-in `bun:sqlite` driver. The entry runs directly on the benchmark VM. TinyJS uses its published production HTTP adapter under Bun.

File-based routes use prepared statements against the challenge's existing indexes. SQLite uses WAL and synchronous NORMAL. Each like runs as a transaction, and token signatures are checked on every authenticated request.

Build with `bash build.sh` and run with `bash start.sh` using the challenge environment variables.

## Publication dependency

The published TinyJS 0.1.0 package mounts these routes below `/api`. The challenge calls root paths such as `/health`. This branch needs a published TinyJS release with opt-in root-mounted API routes before it can pass the official harness. For a local production smoke test of the current published package, run `bun run smoke.ts node_modules/tinyjs-fullstack/bin/tinyjs.js /api` after building.
