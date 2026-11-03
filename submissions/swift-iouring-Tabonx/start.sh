#!/usr/bin/env bash
# Runs the server in the foreground. Configured by SQLITE_PATH, JWT_SECRET, HOST and PORT (SPEC.md);
# HOST and PORT default to direct serving on 0.0.0.0:80.
set -euo pipefail
cd "$(dirname "$0")"
export HOST="${HOST:-0.0.0.0}" PORT="${PORT:-80}"
exec ./bin/server
