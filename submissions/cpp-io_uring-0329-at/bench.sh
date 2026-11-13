#!/bin/bash
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DB=/tmp/d.db
VUS=${VUS:-20000}
RAMP=${RAMP:-30s}
DUR=${DUR:-60s}
PORT=${PORT:-3000}
CURSORS=${SHIM_CURSORS:-1}
SRV_CPU=${SRV_CPU:-0}
K6_CPUS=${K6_CPUS:-1-7}

cd "$HERE" || exit 1
command -v k6 >/dev/null || { echo "k6 not installed"; exit 1; }
ulimit -n 1000000 2>/dev/null || ulimit -n "$(ulimit -Hn)"

live() { ps -eo stat=,comm= | awk '$2=="server" && $1!="Z"' | wc -l; }

echo "== 1. cleanup"
pkill -9 -x server 2>/dev/null
sleep 2
echo "   live=$(live)"

echo "== 2. fresh database"
cp "$ROOT/seed/feed.db" "$DB"
rm -f "$DB-wal" "$DB-shm"

echo "== 3. build"
if ! bash "$HERE/build.sh" >/tmp/build.log 2>&1; then
  echo "   build failed, last 20 lines:"
  tail -20 /tmp/build.log
  exit 1
fi
echo "   ok"

echo "== 4. start (cursors=$CURSORS, server on cpu $SRV_CPU)"
taskset -c "$SRV_CPU" env SHIM_CURSORS=$CURSORS SQLITE_PATH="$DB" \
  JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=$PORT \
  ./server >/tmp/run.log 2>&1 &
sleep 2

if [ "$(live)" -lt 1 ]; then
  echo "   !! server did not start:"
  tail -10 /tmp/run.log
  exit 1
fi
head -2 /tmp/run.log | sed 's/^/   /'
curl -s -m 2 -o /dev/null -w "   health=%{http_code}\n" "http://127.0.0.1:$PORT/health"

PID=$(ps -eo pid=,stat=,comm= | awk '$3=="server" && $2!="Z" {print $1}' | tail -1)
TICKS0=$(awk '{print $14+$15}' /proc/"$PID"/stat)

echo "== 5. correctness (42)"
bash "$ROOT/test/test.sh" "http://127.0.0.1:$PORT" 2>&1 | tail -2

echo "== 6. k6 (VUS=$VUS ramp=$RAMP dur=$DUR, client on cpu $K6_CPUS)"
taskset -c "$K6_CPUS" k6 run -e VUS="$VUS" -e RAMP_UP="$RAMP" -e DURATION="$DUR" \
  "$ROOT/bench/load.js" >/tmp/k6.log 2>&1
if grep -q 'p(95)' /tmp/k6.log; then
  grep -E "p\(95\)|p\(99\)|http_req_failed|http_reqs|checks_failed" /tmp/k6.log
else
  echo "   no summary, last 20 lines:"
  tail -20 /tmp/k6.log
fi

echo "== 7. cpu / memory"
TICKS1=$(awk '{print $14+$15}' /proc/"$PID"/stat)
DT=$((TICKS1 - TICKS0))
REQS=$(grep -E 'http_reqs' /tmp/k6.log | head -1 | grep -oE '[0-9]{4,}' | head -1)
echo "   ticks      = $DT  (1 tick = 10ms, CLK_TCK=$(getconf CLK_TCK))"
if [ -n "${REQS:-}" ] && [ "$REQS" -gt 0 ]; then
  echo "   requests   = $REQS"
  echo "   us/request = $(( DT * 10000 / REQS ))"
else
  echo "   requests   = (not parsed from k6 log)"
fi
awk '/^VmHWM/{printf "   peak RSS   = %.0f MB\n", $2/1024}' /proc/"$PID"/status
awk '/^VmRSS/{printf "   current RSS= %.0f MB\n", $2/1024}' /proc/"$PID"/status
echo "   threads    = $(awk '/^Threads/{print $2}' /proc/"$PID"/status)"
echo "   cpus used  = $(awk '/^Cpus_allowed_list/{print $2}' /proc/"$PID"/status)"

echo "== 8. wal / fallback"
ls -lh "$DB" "$DB-wal" 2>/dev/null | awk '{printf "   %-6s %s\n", $5, $9}'
n=$(grep -c 'shim:' /tmp/run.log 2>/dev/null || true)
echo "   shim notes = ${n:-0}"

echo "== 9. stop"
pkill -9 -x server 2>/dev/null
