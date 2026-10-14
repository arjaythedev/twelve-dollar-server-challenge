#!/usr/bin/env bash
# Runs the server in the foreground. Reads SQLITE_PATH, JWT_SECRET, HOST and PORT (see SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"

# For /health's uptime_s: the server's start time, shared by every request.
export APP_STARTED_AT="$(date +%s)"

exec frankenphp run --config Caddyfile --adapter caddyfile
