#!/usr/bin/env bash
# PGO training: runs the instrumented server on a synthetic database (never
# the seed) under the load generator for a few seconds, mixing the k6 user
# model with a no-think-time loop so the hot paths get real counts.
#
#   pgo/train.sh build/server_pgo
set -euo pipefail
cd "$(dirname "$0")/.."
SERVER="${1:?instrumented server binary}"
PORT=18181
DB=build/train.db
TOKENS=build/train-tokens.json
[ -x bin/synthdb ] && [ -x bin/mload ] || { echo "pgo: tools missing"; exit 1; }
bin/synthdb "$DB" "$TOKENS" 5000 40000 120000 pgo-training-secret 2000 >/dev/null
SQLITE_PATH="$DB" JWT_SECRET=pgo-training-secret HOST=127.0.0.1 PORT=$PORT \
  "$SERVER" --max-conns 20000 --exit-after-s 40 >build/pgo-server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for _ in $(seq 100); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || { echo "pgo: server not healthy"; cat build/pgo-server.log; exit 1; }
# a realistic mix (think time scaled down 20x so 2000 users produce load) ...
bin/mload --port $PORT --vus 2000 --duration 10 --ramp 2 --think-scale 0.05 --src-ips 1 --tokens "$TOKENS" \
  --report-every 5 >build/pgo-load1.log
# ... and a tight loop to weight the per-request paths
bin/mload --port $PORT --vus 32 --duration 6 --ramp 1 --think-scale 0 --src-ips 1 --tokens "$TOKENS" \
  --report-every 5 >build/pgo-load2.log
# the error paths, so they are not optimized as if they never happen
for i in $(seq 50); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/posts/abc"
  curl -s -o /dev/null -X POST "http://127.0.0.1:$PORT/posts" -d '{"body":""}'
  curl -s -o /dev/null -X POST -H 'Authorization: Bearer x.y.z' "http://127.0.0.1:$PORT/posts/1/like"
  curl -s -o /dev/null "http://127.0.0.1:$PORT/nope"
done
kill -TERM $SRV
wait $SRV || true
trap - EXIT
grep -q 'thresholds' build/pgo-load1.log || { echo "pgo: load run did not finish"; exit 1; }
n=$(find build/prof -name '*.gcda' 2>/dev/null | wc -l)
[ "$n" -gt 0 ] || { echo "pgo: no profile data"; exit 1; }
echo "pgo: $n profile files"
