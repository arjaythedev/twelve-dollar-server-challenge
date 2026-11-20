#!/usr/bin/env bash
# Local approximation of the challenge's scoring run, for one machine.
#
#   K6=/path/to/k6 ECKO=/path/to/ecko bash submissions/ecko-stdhttp-sn/bench-local.sh 2500 [duration]
#
# The server gets one CPU (SERVER_CPU, default 3) and k6 gets the rest (K6_CPUS, default 4-31),
# which stands in for the 1 vCPU droplet with load from other machines. It is not the real box:
# memory is not capped, and the CPU is this machine's. Each probe starts the server on a fresh
# copy of seed/feed.db, as the scoring run does, and reports pass/fail on the same thresholds
# (p95 < 500 ms, p99 < 1 s, errors < 1%), the request rate, and the server's CPU and peak RSS.
set -euo pipefail
VUS="${1:?usage: bench-local.sh <vus> [duration]}"
DURATION="${2:-2m}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
K6="${K6:-k6}"
SERVER_CPU="${SERVER_CPU:-3}"
K6_CPUS="${K6_CPUS:-4-31}"
PORT="${PORT:-3200}"
OUT="${OUT:-$HERE/bench-out}"
mkdir -p "$OUT"

export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT
WORK="$(mktemp -d)"
export SQLITE_PATH="$WORK/feed.db"
cp "$ROOT/seed/feed.db" "$SQLITE_PATH"

taskset -c "$SERVER_CPU" bash "$HERE/start.sh" > "$WORK/server.log" 2>&1 &
PID=$!
cleanup() { pkill -P $PID 2>/dev/null || true; kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT
for _ in $(seq 60); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 1; done
curl -sf "http://$HOST:$PORT/health" >/dev/null || { echo "no healthy /health"; cat "$WORK/server.log"; exit 1; }
SPID="$(pgrep -P $PID || echo $PID)"
ticks0="$(awk '{print $14 + $15}' "/proc/$SPID/stat")"

rc=0
# One source address has ~28k ephemeral ports, which caps a single-IP load test near 28k users;
# the real run uses four load generators. Spread k6 over several loopback addresses instead.
taskset -c "$K6_CPUS" "$K6" run -q --no-color --local-ips="${LOCAL_IPS:-127.0.0.2-127.0.0.9}" \
    -e BASE_URL="http://$HOST:$PORT" -e VUS="$VUS" -e DURATION="$DURATION" -e RAMP_UP="${RAMP_UP:-30s}" \
    -e TOKENS="$ROOT/seed/tokens.json" --summary-export "$OUT/summary-$VUS.json" \
    "$ROOT/bench/load.js" > "$OUT/k6-$VUS.log" 2>&1 || rc=$?

ticks1="$(awk '{print $14 + $15}' "/proc/$SPID/stat")"
hwm="$(awk '/VmHWM/ {print $2}' "/proc/$SPID/status")"
jq -r --arg vus "$VUS" --arg rc "$rc" --arg cpu "$(( (ticks1 - ticks0) / $(getconf CLK_TCK) ))" --arg hwm "$(( hwm / 1024 ))" '
  .metrics as $m |
  "vus=\($vus) \(if $rc == "0" then "PASS" else "FAIL" end) req/s=\($m.http_reqs.rate | floor) " +
  "p95=\($m.http_req_duration["p(95)"] * 10 | floor / 10)ms p99=\($m.http_req_duration["p(99)"] * 10 | floor / 10)ms " +
  "err=\($m.http_req_failed.value * 10000 | floor / 100)% server_cpu_s=\($cpu) server_rss_peak=\($hwm)MB"
' "$OUT/summary-$VUS.json" | tee -a "$OUT/results.txt"
grep -iE 'error|panic|fail' "$WORK/server.log" | sort | uniq -c | head -5 || true
