#!/usr/bin/env bash
# Runs the server in the foreground. Configured by env vars (SPEC.md):
#   SQLITE_PATH, JWT_SECRET, HOST, PORT
# Defaults match direct serving (0.0.0.0:80); the benchmark passes them explicitly.
set -euo pipefail
cd "$(dirname "$0")"

# The binary links the toolchain's shared Swift runtime; make sure it is findable.
if [ -d /opt/swift/usr/lib/swift/linux ]; then
    export LD_LIBRARY_PATH="/opt/swift/usr/lib/swift/linux${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

export HOST="${HOST:-0.0.0.0}"
export PORT="${PORT:-80}"

exec ./bin/server
