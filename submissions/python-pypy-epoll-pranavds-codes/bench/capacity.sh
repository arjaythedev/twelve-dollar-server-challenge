#!/usr/bin/env bash
# User-model capacity: bench/capacity.sh <submission dir> <users> [hold seconds=300] [ramp seconds=60]
# The server runs pinned to CPU 0 on a fresh seed copy. connection_load.c (bench/load.js's user loop:
# feed, 3-7 s, post, 3-8 s, 15% like, 2% create, 5-15 s) runs in shards of <=16k users on CPUs 1-3.
# Pass = p95 < 500 ms, p99 < 1 s, errors < 1% over the hold, like the official score.
set -uo pipefail
DIR=$(cd "$1" && pwd); U=$2; H=${3:-300}; R=${4:-60}
B=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$B/../../.." && pwd)
[ -x "$B/connection-load" ] || cc -O2 -o "$B/connection-load" "$B/connection_load.c" -lm
export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=${PORT:-3100}
T=$(mktemp -d); export SQLITE_PATH=$T/feed.db
cp "$ROOT/seed/feed.db" "$SQLITE_PATH"
taskset -c 0 bash "$DIR/start.sh" > "$T/server.log" 2>&1 &
PID=$!
for _ in $(seq 100); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.2; done
tree() { echo $PID $(pgrep -P $PID) $(for p in $(pgrep -P $PID); do pgrep -P $p; done); }
ticks() { for p in $(tree); do cat /proc/$p/stat 2>/dev/null; done | awk '{u+=$14+$15} END {print u}'; }
rss() { for p in $(tree); do cat /proc/$p/status 2>/dev/null; done | awk '/VmRSS/{s+=$2} END {print int(s/1024)}'; }
SH=$(( (U + 15999) / 16000 )); PER=$(( (U + SH - 1) / SH ))
pids=()
for i in $(seq 0 $((SH-1))); do
  n=$(( i == SH-1 ? U - PER*(SH-1) : PER ))
  taskset -c 1-3 "$B/connection-load" "$PORT" --users "$n" --offset $((i*PER)) --seconds "$H" --ramp "$R" \
    --tokens "$ROOT/seed/tokens.json" > "$T/shard$i.json" 2>&1 &
  pids+=($!)
done
sleep "$R"; c0=$(ticks); peak=0
for _ in $(seq $((H/5))); do sleep 5; r=$(rss); [ "$r" -gt "$peak" ] && peak=$r; done
c1=$(ticks); for p in "${pids[@]}"; do wait "$p"; done
python3 - "$T" "$U" "$H" "$((c1-c0))" "$peak" "$(basename "$DIR")" <<'PY'
import json, glob, sys
T, U, H, ticks, peak, name = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6]
rs = []
for f in sorted(glob.glob(T + "/shard*.json")):
    lines = [l for l in open(f) if l.startswith('{"diagnostic"')]
    if not lines: print("shard failed:", open(f).read()[-500:]); continue
    rs.append(json.loads(lines[-1]))
req = sum(r["requests"] for r in rs); err = sum(r["errors"]["failed_requests"] for r in rs)
p95 = max(r["p95_ms"] for r in rs); p99 = max(r["p99_ms"] for r in rs); rps = sum(r["requests_per_second"] for r in rs)
ok = bool(rs) and err / max(req, 1) < 0.01 and p95 < 500 and p99 < 1000
print(f"{name} users={U} rps={rps:.0f} p95={p95:.1f}ms p99={p99:.1f}ms errors={err}/{req} "
      f"connects={sum(r['connects'] for r in rs)} server_cpu={ticks/H:.0f}% rss_peak={peak}MB {'PASS' if ok else 'FAIL'}")
PY
pkill -P $PID 2>/dev/null; kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$T"
