#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
export JR_TRANSPORT="${JR_TRANSPORT:-$TRANSPORT}"   # dev runs may override it
exec "$JULIA_HOME/bin/julia" --startup-file=no --history-file=no -O3 --heap-size-hint="$JULIA_HEAP_HINT" src/main.jl
