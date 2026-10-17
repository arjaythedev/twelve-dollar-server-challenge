#!/usr/bin/env bash
# confirm.sh <total-vus> <dst-ip> [dst-ip...]: official README confirmation:
# 2-min warmup @1000 VUs, 60s ramp, 5-min hold, 30s ramp-down (in load.js),
# then pass/fail per p95<500ms, p99<1s, errors<1%.
# Usage (on cachyos): bash dev/confirm.sh 70000 127.0.0.2 127.0.0.3 127.0.0.4
set -u
TOTAL=${1:?total-vus}; shift
[ $# -ge 1 ] || exit 1
C=s12
docker exec $C bash -c "pkill -x server 2>/dev/null; pkill -f \"start.s[h]\" 2>/dev/null; true"; sleep 2
docker exec $C bash -c "cp /repo/seed/feed.db /tmp/cap.db && rm -f /tmp/cap.db-wal /tmp/cap.db-shm" || exit 1
docker exec -d $C bash -c "exec env SQLITE_PATH=/tmp/cap.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3100 /sub/bin/server > /tmp/srv.log 2>&1"
for i in $(seq 1 30); do curl -sf http://127.0.0.1:3100/health >/dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:3100/health >/dev/null || { echo "SERVER FAILED"; docker exec $C cat /tmp/srv.log; exit 1; }
echo "server up"
echo "== warmup 1000 VUs / 2m =="
TOKENS=~/12-server/repo/seed/tokens.json taskset -c 8-11 ~/12-server/bin/k6 run \
  -e BASE_URL=http://127.0.0.1:3100 -e VUS=1000 -e RAMP_UP=10s -e DURATION=2m ~/12-server/repo/bench/load.js 2>&1 | tr '\r' '\n' | grep -a -E "checks_failed|http_req_failed\.|✗" | head -6
PER=$(( (TOTAL + $# - 1) / $# ))
echo "== hold $TOTAL / 5m across $* =="
i=0
for ip in "$@"; do
  c0=$(( 8 + 4 * i )); c1=$(( c0 + 3 ))
  n=$PER; [ $(( i * PER + PER )) -gt "$TOTAL" ] && n=$(( TOTAL - i * PER ))
  LOG=/tmp/confirm-$ip.log; rm -f "$LOG"
  ( ulimit -n 200000
    TOKENS=~/12-server/repo/seed/tokens.json taskset -c $c0-$c1 ~/12-server/bin/k6 run \
      -e BASE_URL=http://$ip:3100 -e VUS="$n" -e RAMP_UP=60s -e DURATION=5m ~/12-server/repo/bench/load.js > "$LOG" 2>&1
  ) &
  echo "launched $n VUs -> $ip"
  i=$(( i + 1 ))
done
wait
echo "===================== RESULTS ====================="
for ip in "$@"; do
  echo "----- $ip -----"
  tr '\r' '\n' < /tmp/confirm-$ip.log | grep -a -E "http_req_duration\.|http_req_failed\.|checks_failed|thresholds" | head -5
done
echo "----- server log (evict/errors) -----"
docker exec $C bash -c "grep -a -E \"evict|FAIL|error\" /tmp/srv.log | head -20"
docker exec $C bash -c "pkill -x server 2>/dev/null; true"
