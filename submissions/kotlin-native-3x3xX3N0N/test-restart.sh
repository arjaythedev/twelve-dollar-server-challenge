#!/usr/bin/env bash
# test-restart.sh: a restart right after a stop with open keep-alive connections must come up healthy.
# On Linux 6.8 the old process's io_uring is torn down asynchronously after exit; its pending multishot accept keeps
# the old listening socket alive for a while, so an immediate bind() of the new process got EADDRINUSE.
#   bash test-restart.sh [conns=2000] [port=3099]   (needs build/server.kexe and seed/feed.db; python3)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$DIR/../.." && pwd)"
N="${1:-2000}"; export PORT="${2:-3099}" HOST=127.0.0.1 JWT_SECRET=twelve-dollar-challenge
T="$(mktemp -d)"; export SQLITE_PATH="$T/feed.db"; cp "$ROOT/seed/feed.db" "$SQLITE_PATH"
P=""; H=""
cleanup() { [ -n "$H" ] && kill "$H" 2>/dev/null; [ -n "$P" ] && kill "$P" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
ulimit -n "$(ulimit -Hn)"
start() { "$DIR/build/server.kexe" >> "$T/server.log" 2>&1 & P=$!
  for _ in $(seq 600); do curl -sf "http://$HOST:$PORT/health" >/dev/null && return 0; kill -0 $P 2>/dev/null || return 1; sleep 0.1; done; return 1; }
start || { echo "FAIL: first start"; cat "$T/server.log"; exit 1; }
# hold N keep-alive connections that each got a response (TCP_DEFER_ACCEPT: an idle socket is never accepted)
python3 -I - "$HOST" "$PORT" "$N" <<'EOF' &
import socket, sys, time
h, p, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); ss = []
for i in range(n):
    s = socket.create_connection((h, p)); s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"); ss.append(s)
for s in ss: s.recv(4096)
print("holding", len(ss), flush=True); time.sleep(600)
EOF
H=$!
for _ in $(seq 300); do [ "$(ss -Htn state established "( sport = :$PORT )" | wc -l)" -ge "$N" ] && break; sleep 0.1; done
echo "open: $(ss -Htn state established "( sport = :$PORT )" | wc -l)"
kill -TERM $P; wait $P 2>/dev/null || true; P=""
t0=$(date +%s.%N)
if start; then echo "PASS: restarted healthy in $(python3 -I -c "import time;print(round(time.time()-$t0,2))") s"; grep -i "bind" "$T/server.log" || true
else echo "FAIL: no healthy restart"; tail -5 "$T/server.log"; exit 1; fi
