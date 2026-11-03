#!/usr/bin/env bash
# Runs the server in the foreground. Configured only by SQLITE_PATH,
# JWT_SECRET, HOST and PORT from the environment (see SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
ulimit -Sn "$(ulimit -Hn)" 2>/dev/null || true
exec ./bin/server
