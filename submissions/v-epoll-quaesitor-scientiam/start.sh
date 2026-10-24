#!/usr/bin/env bash
# Runs the server in the foreground. Configuration comes only from SPEC.md's variables:
# SQLITE_PATH, JWT_SECRET, HOST, PORT. SERVER_PROCS is this script's own constant: with
# more than one, that many processes share the port (SO_REUSEPORT) and the database (WAL),
# each with its own descriptor table; the script exits when any of them does.
set -euo pipefail
cd "$(dirname "$0")"
SERVER_PROCS="${SERVER_PROCS:-1}"
if [ "$SERVER_PROCS" -le 1 ]; then
  exec bin/server
fi
export SERVER_PROCS
pids=()
for _ in $(seq 1 "$SERVER_PROCS"); do
  bin/server &
  pids+=($!)
done
trap 'kill "${pids[@]}" 2>/dev/null; wait' EXIT
wait -n
exit 1
