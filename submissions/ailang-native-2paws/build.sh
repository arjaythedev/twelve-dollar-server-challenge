#!/usr/bin/env bash
# Build feed_server.x from source with the AILANG compiler that install.sh puts in
# /usr/local/bin. Set AILANG_COMPILER to use a different one.
set -euo pipefail
cd "$(dirname "$0")"
COMPILER="${AILANG_COMPILER:-/usr/local/bin/ailang.x}"
if [[ ! -x "$COMPILER" ]]; then
  echo "FAIL missing compiler: $COMPILER (run install.sh, or set AILANG_COMPILER)" >&2
  exit 1
fi
export AILANG_LIB_PATH="$PWD/Librarys"
"$COMPILER" feed_server.ailang -o feed_server.x
