#!/usr/bin/env bash
# saturate.sh <container> <subpath> <port> <label>: 2x20s mixed saturation.
# Kills servers AND resurrection loops before/after. Server must be prebuilt.
# Usage (on cachyos): bash dev/saturate.sh s12 /sub 3100 ours
set -u
C=${1:?container}; SUB=${2:?subpath}; PORT=${3:?port}; LABEL=${4:?label}
docker exec "$C" bash -c "pkill -x server 2>/dev/null; pkill -f twelve-rust-po[ll] 2>/dev/null; pkill -f \"start.s[h]\" 2>/dev/null; true"; sleep 2
docker exec "$C" bash -c "cp /repo/seed/feed.db /tmp/s.db && rm -f /tmp/s.db-wal /tmp/s.db-shm" || exit 1
docker exec -d "$C" bash -c "exec env SQLITE_PATH=/tmp/s.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=$PORT bash $SUB/start.sh > /tmp/srv.log 2>&1"
for i in $(seq 1 25); do curl -sf http://127.0.0.1:$PORT/health >/dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:$PORT/health >/dev/null || { echo "$LABEL FAILED TO START"; docker exec "$C" cat /tmp/srv.log; exit 1; }
for r in 1 2; do
  OUT=$(TOKENS=~/12-server/repo/seed/tokens.json taskset -c 8-15 ~/12-server/bin/k6 run --vus 64 --duration 20s -e BASE_URL=http://127.0.0.1:$PORT ~/12-server/work/ours/dev/saturation.js 2>&1)
  RPS=$(echo "$OUT" | grep -oE "[0-9]+\.[0-9]+/s" | head -1)
  ERR=$(echo "$OUT" | grep -a -oE "http_req_failed[^%]*%[^0-9]*[0-9.]+%" | head -1)
  echo "$LABEL run$r: $RPS ($ERR)"
done
docker exec "$C" bash -c "pkill -x server 2>/dev/null; pkill -f twelve-rust-po[ll] 2>/dev/null; pkill -f \"start.s[h]\" 2>/dev/null; true"
