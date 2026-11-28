#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
export JR_TRANSPORT="${JR_TRANSPORT:-$TRANSPORT}"   # dev runs may override it
# The ahead-of-time build from build.sh, if there is one (JR_AOT=0: always the JIT, for dev A/B).
if [ "${JR_AOT:-1}" != 0 ] && [ -x build/server ]; then
  exec build/server
fi
exec "$JULIA_HOME/bin/julia" --startup-file=no --history-file=no -O3 --heap-size-hint="$JULIA_HEAP_HINT" src/main.jl
