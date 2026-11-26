#!/usr/bin/env bash
# test-rings.sh [conns=2500] [port=3099]: the ring spill must keep accepting. KN_TABLE=600 makes every io_uring hold
# 344 connections (600 - FD_MARGIN 256), so 2,500 keep-alive connections need 8 rings; every connection must get its
# /health answer. (A 6.8 kernel VM stopped accepting once a third ring was opened: 130,528 of 192,000 connections.)
#   bash test-rings.sh   (needs build/server.kexe and seed/feed.db; python3)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$DIR/../.." && pwd)"
N="${1:-2500}"; export PORT="${2:-3099}" HOST=127.0.0.1 JWT_SECRET=twelve-dollar-challenge KN_TABLE="${KN_TABLE:-600}"
T="$(mktemp -d)"; export SQLITE_PATH="$T/feed.db"; cp "$ROOT/seed/feed.db" "$SQLITE_PATH"
P=""
cleanup() { [ -n "$P" ] && kill "$P" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
ulimit -n "$(ulimit -Hn)"
"$DIR/build/server.kexe" > "$T/server.log" 2>&1 & P=$!
for _ in $(seq 100); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.1; done
rc=0
python3 -I - "$HOST" "$PORT" "$N" <<'EOF' || rc=$?
import socket, sys, time
h, p, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); ss = []; ok = 0
for i in range(n):
    s = socket.create_connection((h, p)); s.settimeout(10); ss.append(s)
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
    try:
        if s.recv(4096).startswith(b"HTTP/1.1 200"): ok += 1
        else: print("bad answer on", i); break
    except socket.timeout:
        print(f"no answer on connection {i} (first {ok} answered)"); break
# the first connections must still be served after the spill
for s in ss[:10] if ok == n else []:
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"); ok += s.recv(4096).startswith(b"HTTP/1.1 200")
print(f"answered {ok} of {n + 10}"); sys.exit(0 if ok == n + 10 else 1)
EOF
grep -E "ring|accept" "$T/server.log" | sort | uniq -c | head -12
[ $rc = 0 ] && echo PASS || { echo FAIL; exit 1; }
