#!/usr/bin/env bash
# Runs the server in the foreground: PROCESSES copies (4 unless given) on the same port, which Linux shares the
# connections between (SO_REUSEPORT). Each process may only have 65,535 open files (LimitNOFILE), and every user of the
# load test holds a keep-alive connection, so one process couldn't hold more than about 65,000 users. The first process
# makes the checkpoints of WAL for all. If a process ends, the others are stopped and the script fails
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COUNT="${PROCESSES:-4}"
pids=()
for i in $(seq 1 "$COUNT"); do
    checkpoints=0
    [ "$i" = 1 ] && checkpoints=1
    CHECKPOINTS=$checkpoints "$HERE/bin/server" &
    pids+=($!)
done
trap 'kill "${pids[@]}" 2>/dev/null; wait' EXIT INT TERM
wait -n
exit 1
