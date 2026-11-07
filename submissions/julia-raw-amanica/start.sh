#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
exec "$JULIA_HOME/bin/julia" --startup-file=no --history-file=no -O3 src/main.jl
