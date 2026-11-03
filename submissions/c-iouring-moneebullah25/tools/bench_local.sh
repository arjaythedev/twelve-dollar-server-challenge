#!/usr/bin/env bash
# One reproducible local measurement: starts a server on a fresh seed copy,
# runs mload against it, and prints the server's own stats (CPU us/request,
# RSS, checkpoints, holds) next to the generator's latency summary.
#
#   tools/bench_local.sh [--server bin/server] [--vus N] [--duration S] [--ramp S]
#                        [--think-scale F] [--src-ips N] [--mem-max 2G] [--label text]
#                        [-- extra server flags...]
set -euo pipefail
cd "$(dirname "$0")/.."
SERVER=bin/server; VUS=1000; DURATION=60; RAMP=10; THINK=1; SRC_IPS=8; MEM_MAX=""; LABEL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --server) SERVER=$2; shift 2 ;;
    --vus) VUS=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --ramp) RAMP=$2; shift 2 ;;
    --think-scale) THINK=$2; shift 2 ;;
    --src-ips) SRC_IPS=$2; shift 2 ;;
    --mem-max) MEM_MAX=$2; shift 2 ;;
    --label) LABEL=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
SEED=../../seed/feed.db
TOKENS=../../seed/tokens.json
[ -f "$SEED" ] && [ -f "$TOKENS" ] || { echo "need $SEED and $TOKENS"; exit 1; }
RUN="$(mktemp -d)"
cp "$SEED" "$RUN/feed.db"
PORT=$((20000 + RANDOM % 20000))
export SQLITE_PATH="$RUN/feed.db" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$PORT
STATS_EVERY=$(( DURATION / 2 > 0 ? DURATION / 2 : 1 ))
if [ -n "$MEM_MAX" ]; then
  systemd-run --user --scope --quiet -p MemoryMax="$MEM_MAX" -- "$SERVER" --stats-interval-s "$STATS_EVERY" "$@" >"$RUN/server.log" 2>&1 &
else
  "$SERVER" --stats-interval-s "$STATS_EVERY" "$@" >"$RUN/server.log" 2>&1 &
fi
SRV=$!
trap 'kill $SRV 2>/dev/null || true; wait $SRV 2>/dev/null || true; rm -rf "$RUN"' EXIT
for _ in $(seq 300); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || { echo "server not healthy"; cat "$RUN/server.log"; exit 1; }
echo "== $LABEL | $(date -u +%Y-%m-%dT%H:%M:%SZ) | server=$SERVER flags='$*' | mload vus=$VUS duration=$DURATION ramp=$RAMP think=$THINK"
grep -E 'config:|rlimit|cgroup' "$RUN/server.log" | sed 's/^/   /'
python3 tools/probe.py "$PORT" $((DURATION + 5)) 0.05 /feed &
PROBE=$!
bin/mload --port "$PORT" --vus "$VUS" --duration "$DURATION" --ramp "$RAMP" --think-scale "$THINK" \
  --src-ips "$SRC_IPS" --tokens "$TOKENS" --report-every "$STATS_EVERY" | sed 's/^/   /'
kill -TERM $PROBE 2>/dev/null || true; wait $PROBE 2>/dev/null || true
free -m | sed -n 2p | sed 's/^/   /'
grep -E 'TCP:|sockets' /proc/net/sockstat | tr '\n' ' ' | sed 's/^/   /'; echo
kill -TERM $SRV; wait $SRV || true
echo "   server stats (last two lines):"; grep 'stats:' "$RUN/server.log" | tail -2 | sed 's/^/   /'
grep -E 'memory guard|ENOBUFS|accept:|fatal' "$RUN/server.log" | head -5 | sed 's/^/   /' || true
trap - EXIT; rm -rf "$RUN"
