#!/usr/bin/env bash
# Local load rig: server and k6 both run inside the Docker Desktop VM.
#
# Why: on WSL2, loopback connections stall ~30 s once a single host drives more
# than roughly 6k concurrent connections (dial: i/o timeout), regardless of k6 or
# the server. Putting both ends on a container network avoids that ceiling.
#
# The server is capped at the challenge box: 1 CPU, 2 GB, no swap, 65535 fds.
# k6 is unconstrained and shares the Docker VM.
#
# usage: K6_BIN=/path/to/k6 dev/loadtest.sh [vus] [ramp] [duration]
#   SCRIPT=/abs/path/to/script.js  optional k6 script (default bench/load.js)
set -euo pipefail

VUS=${1:-10000}
RAMP=${2:-60s}
DUR=${3:-60s}
SUBMISSION_DIR=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$SUBMISSION_DIR/../.." && pwd)
K6_BIN=${K6_BIN:?set K6_BIN to a static linux amd64 k6 binary}
SCRIPT=${SCRIPT:-"$REPO/bench/load.js"}
NET=twelve-bench
PORT=3910

[ -x "$K6_BIN" ] || { echo "K6_BIN is not executable: $K6_BIN" >&2; exit 1; }
[ -f "$REPO/seed/feed.db" ] || { echo "run: bash seed/make-seed.sh" >&2; exit 1; }
[ -f "$SCRIPT" ] || { echo "no k6 script: $SCRIPT" >&2; exit 1; }
[ -x "$SUBMISSION_DIR/bin/server" ] || bash "$SUBMISSION_DIR/build.sh"
docker image inspect ubuntu:24.04 >/dev/null 2>&1 || docker pull ubuntu:24.04

TMP=$(mktemp -d); cp "$REPO/seed/feed.db" "$TMP/feed.db"
OUT=$(mktemp -d); chmod 777 "$OUT"
docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
docker rm -f twelve-server >/dev/null 2>&1 || true

docker run -d --name twelve-server --network "$NET" -p "127.0.0.1:$PORT:3000" \
  --cpus=1 --memory=2g --memory-swap=2g --ulimit nofile=65535:65535 \
  -e SQLITE_PATH=/data/feed.db -e JWT_SECRET=twelve-dollar-challenge \
  -e HOST=0.0.0.0 -e PORT=3000 -e GOMAXPROCS=1 \
  -v "$TMP:/data" -v "$SUBMISSION_DIR/bin/server:/server:ro" \
  ubuntu:24.04 /server >/dev/null

cleanup() {
  echo "-- server cpu.stat: $(docker exec twelve-server cat /sys/fs/cgroup/cpu.stat 2>/dev/null | tr '\n' ' ')"
  echo "-- server memory.peak: $(docker exec twelve-server cat /sys/fs/cgroup/memory.peak 2>/dev/null)"
  docker rm -f twelve-server >/dev/null 2>&1 || true
  rm -rf "$TMP" "$OUT"
}
trap cleanup EXIT

for _ in $(seq 100); do curl -sf -m 1 "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
HEALTH=$(curl -sf -m 2 "http://127.0.0.1:$PORT/health") || { echo "server not healthy"; docker logs twelve-server; exit 1; }
UP=$(printf '%s' "$HEALTH" | sed -n 's/.*"uptime_s":\([0-9]*\).*/\1/p')
echo "health: $HEALTH"
[ -n "$UP" ] && [ "$UP" -le 5 ] || { echo "stale listener on :$PORT"; exit 1; }
echo "limits: $(docker inspect -f 'cpus={{.HostConfig.NanoCpus}} mem={{.HostConfig.Memory}} swap={{.HostConfig.MemorySwap}}' twelve-server)"

docker run --rm --name twelve-k6 --network "$NET" \
  -v "$K6_BIN:/k6:ro" \
  -v "$SCRIPT:/script.js:ro" \
  -v "$REPO/seed/tokens.json:/tokens.json:ro" \
  -v "$OUT:/out" \
  ubuntu:24.04 /k6 run --summary-export=/out/summary.json \
    -e BASE_URL=http://twelve-server:3000 -e TOKENS=/tokens.json \
    -e VUS="$VUS" -e RAMP_UP="$RAMP" -e DURATION="$DUR" /script.js \
    >"$OUT/k6.log" 2>&1
rc=$?
echo "k6 exit=$rc (log: $OUT/k6.log)"
echo -n "failure classes:"
for pat in 'i/o timeout' 'cannot assign requested address' 'connection reset' 'no buffer space' 'connection refused' 'EOF'; do
  printf ' [%s=%s]' "$pat" "$(grep -c "$pat" "$OUT/k6.log" || true)"
done
echo
if command -v jq >/dev/null 2>&1; then
  jq --argjson vus "$VUS" '{vus: $vus, reqs: (.metrics.http_reqs.count // 0),
    rps: (((.metrics.http_reqs.rate // 0)*100)|round/100),
    p95_ms: (((.metrics.http_req_duration["p(95)"] // 0)*100)|round/100),
    p99_ms: (((.metrics.http_req_duration["p(99)"] // 0)*100)|round/100),
    connect_p95_ms: (((.metrics.http_req_connecting["p(95)"] // 0)*100)|round/100),
    failed: (.metrics.http_req_failed.value // 0)}' "$OUT/summary.json"
else
  cat "$OUT/summary.json"
fi
