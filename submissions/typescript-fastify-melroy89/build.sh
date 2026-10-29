#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# CI may put a runner-managed Node/npm ahead of the pinned installation.
export PATH="/usr/local/bin:$PATH"
# Compile the native SQLite binding from source on the benchmark machine.
/usr/local/bin/node /usr/local/lib/node_modules/npm/bin/npm-cli.js ci --ignore-scripts --no-audit --no-fund
/usr/local/bin/node node_modules/node-gyp/bin/node-gyp.js rebuild --release --force_build=1 --directory=node_modules/better-sqlite3
/usr/local/bin/node /usr/local/lib/node_modules/npm/bin/npm-cli.js run build
