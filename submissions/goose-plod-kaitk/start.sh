#!/usr/bin/env bash
# Runs the server in the foreground: PROCS processes, 3 by default, each one
# worker on HOST:PORT through SO_REUSEPORT. Every k6 user holds a connection,
# and one process holds at most LimitNOFILE of them, 65,535 on the box, so
# three take 192,000. They share the database as SQLite's WAL lets processes
# share it. When one exits, start.sh stops the rest and exits, for systemd to
# restart.
set -euo pipefail
cd "$(dirname "$0")"
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT
for _ in $(seq "${PROCS:-3}"); do
    bin/server &
    pids+=($!)
done
wait -n
exit 1
