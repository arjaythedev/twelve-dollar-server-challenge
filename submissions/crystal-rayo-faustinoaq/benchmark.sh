#!/usr/bin/env bash
# Run inside crdev under flock /home/user/experiments/crystal-challenges/bench/.lock.
# Existing reference entry remains untouched; its build runs in our scratch directory.
set -euo pipefail
cd "$(dirname "$0")"
APP=$PWD
FAMILY=$(cd ../../.. && pwd)
SEED=${SEED_DIR:-$FAMILY/twelve-dollar-server-challenge/seed}
REFERENCE=${REFERENCE_DIR:-$FAMILY/twelve-dollar-server-challenge/submissions/crystal-epoll-faustinoaq}
SERVER_CPU=${SERVER_CPU:-9}
CLIENT_CPUS=${CLIENT_CPUS:-10-11}
PORT=${PORT:-18373}
CONNECTIONS=${CONNECTIONS:-256}
DURATION=${DURATION:-15s}
[ "$CONNECTIONS" -le 3000 ]
RESULTS=${RESULTS:-measurements}
mkdir -p build/reference "$RESULTS"
exec > >(tee "${SESSION_FILE:-$RESULTS/session.txt}") 2>&1
date -u +%Y-%m-%dT%H:%M:%SZ
cp -R "$REFERENCE/src" "$REFERENCE/pgo" build/reference/
cp "$REFERENCE/build.sh" "$REFERENCE/start.sh" build/reference/
(cd build/reference && bash build.sh)
PID=
cleanup() {
  if [ -n "$PID" ]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  rm -f build/benchmark.db build/benchmark.db-wal build/benchmark.db-shm
}
trap cleanup EXIT
for workload in ${WORKLOADS:-feed mix users}; do
  for entry in reference rayo; do
    cleanup
    PID=
    cp "$SEED/feed.db" build/benchmark.db
    SUB=$APP
    [ "$entry" != reference ] || SUB=$APP/build/reference
    export SQLITE_PATH=$APP/build/benchmark.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT
    taskset -c "$SERVER_CPU" bash "$SUB/start.sh" > "$RESULTS/$entry-$workload-server.log" 2>&1 &
    PID=$!
    for _ in $(seq 60); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.5; done
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null
    if [ "$workload" = users ]; then
      echo "$entry users=1000 think-scale=0.01 cpu=$SERVER_CPU clients=$CLIENT_CPUS"
      taskset -c "$CLIENT_CPUS" "$FAMILY/bench/ul12/ul12" -port "$PORT" -ips 1 -users 1000 -think-scale 0.01 -ramp 5s -measure 20s -tokens "$SEED/tokens.json" | tee "$RESULTS/$entry-$workload.txt"
      grep -q "pass=true" "$RESULTS/$entry-$workload.txt"
    else
      echo "$entry $workload connections=$CONNECTIONS cpu=$SERVER_CPU clients=$CLIENT_CPUS"
      taskset -c "$CLIENT_CPUS" wrk -t2 -c"$CONNECTIONS" -d3s -s "$FAMILY/bench/$workload.lua" "http://127.0.0.1:$PORT/" > "$RESULTS/$entry-$workload-warmup.txt"
      awk '/Rss:/{print "warm RSS KiB: " $2}' "/proc/$PID/smaps_rollup"
      taskset -c "$CLIENT_CPUS" wrk --latency -t2 -c"$CONNECTIONS" -d"$DURATION" -s "$FAMILY/bench/$workload.lua" "http://127.0.0.1:$PORT/" | tee "$RESULTS/$entry-$workload.txt"
    fi
    awk '/Rss:/{print "final RSS KiB: " $2}' "/proc/$PID/smaps_rollup"
    cleanup
    PID=
  done
done
