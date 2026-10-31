#!/usr/bin/env bash
# start.sh: one ngspice process, in the foreground; nic.so (inside it) listens on HOST:PORT.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${SQLITE_PATH:?}" "${JWT_SECRET:?}" "${HOST:=127.0.0.1}" "${PORT:=3000}"
export SQLITE_PATH JWT_SECRET HOST PORT
exec ngspice -b "$here/build/board.cir"
