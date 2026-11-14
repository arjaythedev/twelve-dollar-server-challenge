#!/usr/bin/env bash
# Memory stability check: drives tests/hold.js against a running server and samples the
# server's RSS every 10 s. RSS must plateau; a steady climb is a leak.
#   usage: tests/leak_check.sh <server-pid> [base-url] [vus] [duration]
set -euo pipefail
cd "$(dirname "$0")"
PID="${1:?server pid}"
BASE="${2:-http://127.0.0.1:3000}"
VUS="${3:-1000}"
DURATION="${4:-2m}"

rss_kb() {
  if [ -r "/proc/$PID/status" ]; then
    awk '/^VmRSS/ {print $2}' "/proc/$PID/status"
  else
    ps -o rss= -p "$PID" | tr -d ' '
  fi
}

k6 run --quiet -e BASE_URL="$BASE" -e VUS="$VUS" -e DURATION="$DURATION" hold.js > k6.log 2>&1 &
K6=$!
echo "t_s rss_kb"
t=0
while kill -0 "$K6" 2>/dev/null; do
  echo "$t $(rss_kb)"
  sleep 10
  t=$((t + 10))
done
wait "$K6" || { echo "k6 failed:"; tail -20 k6.log; exit 1; }
tail -15 k6.log
