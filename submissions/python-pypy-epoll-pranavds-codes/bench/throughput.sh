#!/usr/bin/env bash
# Closed-loop throughput on one server CPU: bench/throughput.sh <submission dir> [mix|feed|post|like|create] [connections] [seconds]
# The server runs pinned to CPU 0 on a fresh seed copy; wrk runs on CPUs 1-3. Set CLOSE=close to send
# "Connection: close" on every request (one TCP connection per request). Needs wrk and seed/feed.db.
set -uo pipefail
DIR=$(cd "$1" && pwd); MODE=${2:-mix}; C=${3:-256}; D=${4:-15}
B=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$B/../../.." && pwd)
export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=${PORT:-3100} TOKENS=$ROOT/seed/tokens.json
T=$(mktemp -d); export SQLITE_PATH=$T/feed.db
cp "$ROOT/seed/feed.db" "$SQLITE_PATH"
taskset -c 0 bash "$DIR/start.sh" > "$T/server.log" 2>&1 &
PID=$!
for _ in $(seq 100); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.2; done
tree() { echo $PID $(pgrep -P $PID) $(for p in $(pgrep -P $PID); do pgrep -P $p; done); }
ticks() { for p in $(tree); do cat /proc/$p/stat 2>/dev/null; done | awk '{u+=$14; s+=$15} END {print u, s}'; }
rss() { for p in $(tree); do cat /proc/$p/status 2>/dev/null; done | awk '/VmRSS/{s+=$2} END {print int(s/1024)}'; }
cd "$B"
taskset -c 1-3 wrk -t3 -c"$C" -d3s -s mix.lua "http://$HOST:$PORT" -- "$MODE" ${CLOSE:-} >/dev/null 2>&1   # warm-up
read -r u0 s0 < <(ticks)
OUT=$(taskset -c 1-3 wrk -t3 -c"$C" -d"${D}s" -s mix.lua "http://$HOST:$PORT" -- "$MODE" ${CLOSE:-} 2>&1)
read -r u1 s1 < <(ticks)
R=$(echo "$OUT" | grep RESULT | sed 's/RESULT //')
rps=$(echo "$R" | sed 's/.*rps=\([0-9]*\).*/\1/')
echo "$(basename "$DIR") $MODE c=$C $R cpu=$(( (u1-u0+s1-s0)/D ))% usr_us=$(awk -v t=$((u1-u0)) -v r="$rps" -v d="$D" 'BEGIN{printf "%.1f", t*10000/(r*d)}') sys_us=$(awk -v t=$((s1-s0)) -v r="$rps" -v d="$D" 'BEGIN{printf "%.1f", t*10000/(r*d)}') rss=$(rss)MB"
pkill -P $PID 2>/dev/null; kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$T"
