#!/usr/bin/env bash
# Build feed_server.x from source. Requires the optimizing compiler (e.g. ailang-opt11.x).
# The standard ./ailang.x cannot compile Library.Hash. A copy of the optimizing compiler is
# bundled in compiler/; set AILANG_COMPILER to use a different one.
set -euo pipefail
cd "$(dirname "$0")"
COMPILER="${AILANG_COMPILER:-$PWD/compiler/ailang-opt11.x}"
if [[ ! -x "$COMPILER" ]]; then
  echo "FAIL missing compiler: $COMPILER (set AILANG_COMPILER)" >&2
  exit 1
fi
export AILANG_LIB_PATH="$PWD/Librarys"
"$COMPILER" feed_server.ailang -o feed_server.x
