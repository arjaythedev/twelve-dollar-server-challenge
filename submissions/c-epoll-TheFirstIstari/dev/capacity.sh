#!/usr/bin/env bash
# capacity.sh <total-vus> <ramp_s> <hold_s> <dst-ip> [dst-ip...]
# Official-protocol capacity probe. Fresh DB + one server (0.0.0.0:3000) in the
# droplet container; one k6 instance per dst IP on host cores (Linux gives each
# dst its own 28k ephemeral-port space, so N instances x 28k users).
# Usage (on cachyos): bash dev/capacity.sh 70000 60 120 127.0.0.2 127.0.0.3 127.0.0.4
set -u
TOTAL=${1:?total-vus}; RAMP=${2:?ramp_s}; HOLD=${3:?hold_s}; shift 3
[ $# -ge 1 ] || { echo "need at least one dst ip"; exit 1; }
C=s12
# kill servers AND start.sh resurrection loops (bracket trick avoids self-match)
docker exec $C bash -c "pkill -x server 2>/dev/null; pkill -f twelve-rust-po[ll] 2>/dev/null; pkill -f \"start.s[h]\" 2>/dev/null; true"; sleep 2
docker exec $C bash -c "cp /repo/seed/feed.db /tmp/cap.db && rm -f /tmp/cap.db-wal /tmp/cap.db-shm" || exit 1
docker exec -d $C bash -c "exec env SQLITE_PATH=/tmp/cap.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3100 /sub/bin/server > /tmp/srv.log 2>&1"
for i in $(seq 1 30); do curl -sf http://127.0.0.1:3100/health >/dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:3100/health >/dev/null || { echo "SERVER FAILED TO START"; docker exec $C cat /tmp/srv.log; exit 1; }
echo "server up: $(curl -s http://127.0.0.1:3100/health)"
echo "== $TOTAL VUs across $# k6 instances (ramp=${RAMP}s hold=${HOLD}s) =="
PER=$(( (TOTAL + $# - 1) / $# ))
i=0
for ip in "$@"; do
  c0=$(( 8 + 3 * i )); c1=$(( c0 + 2 ))
  n=$PER; [ $(( i * PER + PER )) -gt "$TOTAL" ] && n=$(( TOTAL - i * PER ))
  [ "$n" -le 0 ] && break
  LOG=/tmp/k6cap-$ip.log
  rm -f "$LOG"
  ( ulimit -n 200000
    cd ~/12-server/repo/bench
    TOKENS=~/12-server/repo/seed/tokens.json taskset -c $c0-$c1 ~/12-server/bin/k6 run \
      -e BASE_URL=http://$ip:3100 -e VUS="$n" -e RAMP_UP="${RAMP}s" -e DURATION="${HOLD}s" load.js > "$LOG" 2>&1
  ) &
  echo "launched $n VUs -> $ip (cores $c0-$c1, log $LOG)"
  i=$(( i + 1 ))
done
wait
echo; echo "===================== SUMMARIES ====================="
for ip in "$@"; do
  echo "----- $ip -----"
  grep -E "http_req_duration |http_req_failed |http_reqs |checks_failed" /tmp/k6cap-$ip.log | head -6
done
docker exec $C bash -c "pkill -x server 2>/dev/null; pkill -f twelve-rust-po[ll] 2>/dev/null; pkill -f \"start.s[h]\" 2>/dev/null; true"
