#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
runtime=/opt/twelve-fastify-node-v26.11.1
export PATH="$runtime/bin:$PATH"
exec "$runtime/bin/node" dist/server.js
