#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Compile the native SQLite binding from source on the benchmark machine.
npm ci --ignore-scripts --no-audit --no-fund
node node_modules/node-gyp/bin/node-gyp.js rebuild --release --force_build=1 --directory=node_modules/better-sqlite3
npm run build
