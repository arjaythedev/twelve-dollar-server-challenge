#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:$PATH"
exec /usr/local/bin/node dist/server.js
