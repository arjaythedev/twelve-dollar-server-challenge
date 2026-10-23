#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -n "${TRUX:-}" ]; then
  compiler="$TRUX"
elif [ -x ../../trux/bin/trux ]; then
  compiler=../../trux/bin/trux
elif [ -x ../../trux/bin/trux.exe ]; then
  compiler=../../trux/bin/trux.exe
else
  compiler=trux
fi
mkdir -p bin
"$compiler" build -o bin/server server.tx
