#!/usr/bin/env bash
# A/B harness for local measurements (not the challenge score): runs bin/server in a
# 1 CPU / 2 GB Ubuntu container on a fresh seed copy, drives tests/hold.js from a sibling
# k6 container, and reports the app's CPU time per request from the cgroup counters.
#   usage: tests/bench.sh <label> [VUS] [DURATION]
#   env:   SERVER_ENV="FEED_RANGE=0 ..."  extra environment for the server
#          BUILD=1                        rebuild bin/server in the container first (VFLAGS honoured)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
MYSUB="submissions/$(basename "$(dirname "$HERE")")"
SUB="${SUB:-$MYSUB}"
LABEL="${1:?label}"
VUS="${2:-2000}"
DURATION="${3:-60s}"
IMAGE="${IMAGE:-twelve-v:latest}"
mkdir -p "$HERE/../build"

if [ "${BUILD:-0}" = "1" ]; then
  docker run --rm --security-opt seccomp=unconfined -v "$ROOT":/work -w "/work/$SUB" -e VFLAGS="${VFLAGS:-}" -e URING="${URING:-1}" "$IMAGE" \
    bash -c "rm -f bin/server && bash build.sh ${BUILD_ARGS:-}" >"$HERE/../build/build-$LABEL.log" 2>&1
fi

docker rm -f twelve-srv >/dev/null 2>&1 || true
docker run -d --name twelve-srv --cpus=1 --memory=2g --memory-swap=2g --security-opt seccomp=unconfined \
  --ulimit nofile=65535:65535 -v "$ROOT":/work -w "/work/$SUB" "$IMAGE" \
  bash -c "cp /work/seed/feed.db /tmp/feed.db && exec env ${SERVER_ENV:-} SQLITE_PATH=/tmp/feed.db \
    JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash start.sh" >/dev/null
for _ in $(seq 1 60); do
  docker exec twelve-srv bash -c 'exec 3<>/dev/tcp/127.0.0.1/3000 && printf "GET /health HTTP/1.1\r\nHost: x\r\n\r\n" >&3 && head -c 12 <&3 | grep -q "200"' 2>/dev/null && break
  sleep 1
done
cpu0=$(docker exec twelve-srv awk '/usage_usec/{print $2}' /sys/fs/cgroup/cpu.stat)
usr0=$(docker exec twelve-srv awk '/user_usec/{print $2}' /sys/fs/cgroup/cpu.stat)

docker run --rm --network container:twelve-srv --ulimit nofile=65535:65535 \
  -v "$ROOT":/work -w "/work/$MYSUB/tests" grafana/k6:latest run --quiet \
  -e BASE_URL=http://127.0.0.1:3000 -e VUS="$VUS" -e DURATION="$DURATION" \
  -e TOKENS=/work/seed/tokens.json --summary-trend-stats="avg,med,p(95),p(99),max" --summary-export="/work/$MYSUB/build/k6-$LABEL.json" \
  hold.js >"$HERE/../build/k6-$LABEL.log" 2>&1 || true

cpu1=$(docker exec twelve-srv awk '/usage_usec/{print $2}' /sys/fs/cgroup/cpu.stat)
usr1=$(docker exec twelve-srv awk '/user_usec/{print $2}' /sys/fs/cgroup/cpu.stat)
peak=$(docker exec twelve-srv cat /sys/fs/cgroup/memory.peak 2>/dev/null || echo 0)
rss=$(docker exec twelve-srv bash -c 't=0; for p in $(pgrep -x server); do r=$(awk "/VmRSS/{print \$2}" /proc/$p/status); t=$((t+r)); done; echo $t')
J="$HERE/../build/k6-$LABEL.json"
reqs=$(jq -r '.metrics.http_reqs.count' "$J")
p95=$(jq -r '.metrics.http_req_duration["p(95)"]' "$J")
p99=$(jq -r '.metrics.http_req_duration["p(99)"] // .metrics.http_req_duration["p(90)"]' "$J")
err=$(jq -r '.metrics.http_req_failed.value' "$J")
rate=$(jq -r '.metrics.http_reqs.rate' "$J")
printf '%s: reqs=%s (%.0f/s) app_us/req=%d (user %d) p95=%.2fms p99=%.2fms err=%.4f rss=%dMB cgroup_peak=%dMB\n' \
  "$LABEL" "$reqs" "$rate" $(( (cpu1 - cpu0) / (reqs > 0 ? reqs : 1) )) $(( (usr1 - usr0) / (reqs > 0 ? reqs : 1) )) "$p95" "$p99" "$err" \
  $(( rss / 1024 )) $(( peak / 1048576 ))
docker logs twelve-srv > "$HERE/../build/server-$LABEL.log" 2>&1 || true
docker rm -f twelve-srv >/dev/null 2>&1 || true
