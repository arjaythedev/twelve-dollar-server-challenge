#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Use an isolated runtime and npm dependency tree, including in lifecycle scripts.
runtime=/opt/twelve-fastify-node-v26.11.1
export PATH="$runtime/bin:$PATH"
# Compile the native SQLite binding from source on the benchmark machine.
"$runtime/bin/node" "$runtime/lib/node_modules/npm/bin/npm-cli.js" ci --ignore-scripts --no-audit --no-fund
"$runtime/bin/node" node_modules/node-gyp/bin/node-gyp.js rebuild --release --force_build=1 --directory=node_modules/better-sqlite3
"$runtime/bin/node" "$runtime/lib/node_modules/npm/bin/npm-cli.js" run build
