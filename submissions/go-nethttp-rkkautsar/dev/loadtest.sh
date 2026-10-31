#!/usr/bin/env bash
# Local load rig: server and k6 both run inside the Docker Desktop VM.
#
# Why: on WSL2, loopback connections stall ~30 s once a single host drives more
# than roughly 6k concurrent connections (dial: i/o timeout), regardless of k6 or
# the server. Putting both ends on a container network avoids that ceiling.
#
# Server: CPU 0, 1 CPU quota, 2 GB, no swap, 65535 fds.
# k6: CPUs 1..(nproc-3), 3 GB each. The last two CPUs are left to WSL/Docker
# so a load run cannot make the host unresponsive. Override with
# SERVER_CPUSET/K6_CPUSET/K6_MEMORY; LOAD_MAX (default nproc+2) aborts k6 if
# host load runs away.
#
# usage: K6_BIN=/path/to/k6 dev/loadtest.sh [vus] [ramp] [duration]
#   SCRIPT=/abs/path/to/script.js  optional k6 script (default bench/load.js)
#   K6_PROCS=N                     run N k6 containers, each with its own
#                                  source IP (needed above ~20k VUs, since one
#                                  container only gets the VM's ~28k ports)
set -euo pipefail

VUS=${1:-10000}
RAMP=${2:-60s}
DUR=${3:-60s}
PROCS=${K6_PROCS:-1}
NPROC=$(nproc)
SERVER_CPUSET=${SERVER_CPUSET:-0}
K6_CPUSET=${K6_CPUSET:-1-$((NPROC-3))}
K6_MEMORY=${K6_MEMORY:-3g}
LOAD_MAX=${LOAD_MAX:-$((NPROC + 2))}

# One k6 container only has the VM's ~28k ephemeral ports. Beyond ~20k VUs
# it runs out of source ports and turns into a reconnect storm that can take
# the whole host down. Require multiple generator containers above that.
if [ "$PROCS" -eq 1 ] && [ "$VUS" -gt 20000 ]; then
  echo "refusing: VUS=$VUS in one k6 container (port ceiling ~28k); set K6_PROCS>=2" >&2
  exit 1
fi
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
  --cpus=1 --cpuset-cpus="$SERVER_CPUSET" --memory=2g --memory-swap=2g \
  --ulimit nofile=65535:65535 \
  -e SQLITE_PATH=/data/feed.db -e JWT_SECRET=twelve-dollar-challenge \
  -e HOST=0.0.0.0 -e PORT=3000 -e GOMAXPROCS=1 \
  -v "$TMP:/data" -v "$SUBMISSION_DIR/bin/server:/server:ro" \
  ubuntu:24.04 /server >/dev/null

cleanup() {
  echo "-- server cpu.stat: $(docker exec twelve-server cat /sys/fs/cgroup/cpu.stat 2>/dev/null | tr '\n' ' ')"
  echo "-- server memory.peak: $(docker exec twelve-server cat /sys/fs/cgroup/memory.peak 2>/dev/null)"
  for i in $(seq 0 $((PROCS-1))); do
    cp "$OUT/k6-$i.log" "/tmp/k6-inner-$i.log" 2>/dev/null || true
    docker rm -f "twelve-k6-$i" >/dev/null 2>&1 || true
  done
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

WATCH_PID=""
if [ "$LOAD_MAX" -gt 0 ]; then
  (
    while sleep 2; do
      load=$(awk '{print $1}' /proc/loadavg)
      if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN{exit !(l>m)}'; then
        echo "host load $load > $LOAD_MAX; aborting k6" >&2
        for i in $(seq 0 $((PROCS-1))); do
          docker rm -f "twelve-k6-$i" >/dev/null 2>&1 || true
        done
        break
      fi
    done
  ) &
  WATCH_PID=$!
fi

PER=$((VUS / PROCS))
REM=$((VUS % PROCS))
K6_PIDS=()
for i in $(seq 0 $((PROCS-1))); do
  n=$PER; [ "$i" -lt "$REM" ] && n=$((n+1))
  docker run --rm --name "twelve-k6-$i" --network "$NET" \
    --cpuset-cpus="$K6_CPUSET" --memory="$K6_MEMORY" --memory-swap="$K6_MEMORY" \
    -v "$K6_BIN:/k6:ro" \
    -v "$SCRIPT:/script.js:ro" \
    -v "$REPO/seed/tokens.json:/tokens.json:ro" \
    -v "$OUT:/out" \
    ubuntu:24.04 /k6 run --summary-export="/out/summary-$i.json" \
      -e BASE_URL=http://twelve-server:3000 -e TOKENS=/tokens.json \
      -e VUS="$n" -e RAMP_UP="$RAMP" -e DURATION="$DUR" /script.js \
      >"$OUT/k6-$i.log" 2>&1 &
  K6_PIDS+=($!)
done

RCS=()
for p in "${K6_PIDS[@]}"; do
  rc=0; wait "$p" || rc=$?; RCS+=("$rc")
done
[ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null || true
echo "k6 exits: ${RCS[*]} (logs: /tmp/k6-inner-*.log)"
echo -n "failure classes:"
for pat in 'i/o timeout' 'cannot assign requested address' 'connection reset' 'no buffer space' 'connection refused' 'EOF'; do
  printf ' [%s=%s]' "$pat" "$(cat "$OUT"/k6-*.log 2>/dev/null | grep -c "$pat" || true)"
done
echo
if command -v jq >/dev/null 2>&1 && ls "$OUT"/summary-*.json >/dev/null 2>&1; then
  jq -s --argjson vus "$VUS" '{vus: $vus,
    reqs: (map(.metrics.http_reqs.count) | add),
    rps: ((map(.metrics.http_reqs.rate) | add) * 100 | round / 100),
    failed: (map(.metrics.http_req_failed.value * .metrics.http_reqs.count) | add),
    per_process: map({reqs: .metrics.http_reqs.count,
      failed: (.metrics.http_req_failed.value * .metrics.http_reqs.count),
      p95_ms: ((.metrics.http_req_duration["p(95)"] * 100) | round / 100),
      p99_ms: ((.metrics.http_req_duration["p(99)"] * 100) | round / 100)})}' "$OUT"/summary-*.json
else
  cat "$OUT"/summary-*.json
fi
