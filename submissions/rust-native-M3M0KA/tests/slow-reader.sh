#!/usr/bin/env bash
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PORT=${SLOW_READER_PORT:-3411}
WORK=$(mktemp -d)
SERVER=${FEED_BIN:-$HERE/bin/feed}
TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["token"])' "$ROOT/seed/tokens.json")
PID=

cleanup() {
  if [[ -n "$PID" ]]; then
    pkill -TERM -P "$PID" 2>/dev/null || true
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  if [[ -n "${SLOW_READER_LOG:-}" && -f "$WORK/server.log" ]]; then
    cp "$WORK/server.log" "$SLOW_READER_LOG"
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

cp "$ROOT/seed/feed.db" "$WORK/feed.db"
if [[ "${PROTOCOL_TRANSPORT:-epoll}" == l2 ]]; then
  SQLITE_PATH="$WORK/feed.db" JWT_SECRET=twelve-dollar-challenge \
    HOST=127.0.0.1 PORT="$PORT" FEED_L2_URING=1 FEED_WORKERS=1 \
    "$SERVER" >"$WORK/server.log" 2>&1 &
else
  SQLITE_PATH="$WORK/feed.db" JWT_SECRET=twelve-dollar-challenge \
    HOST=127.0.0.1 PORT="$PORT" FEED_EPOLL=1 FEED_EPOLL_INPUT_POOL=1 \
    FEED_WORKERS="${FEED_WORKERS:-1}" "$SERVER" >"$WORK/server.log" 2>&1 &
fi
PID=$!
for _ in $(seq 1 200); do
  if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null; then break; fi
  state=$(ps -o stat= -p "$PID" 2>/dev/null || true)
  if [[ -z "$state" || "$state" == Z* ]]; then
    wait "$PID" 2>/dev/null || true
    cat "$WORK/server.log"
    exit 1
  fi
  sleep 0.1
done
if [[ "${PROTOCOL_TRANSPORT:-epoll}" == l2 ]]; then
  grep -Fq 'transport: l2 io_uring' "$WORK/server.log"
else
  grep -Fq 'transport: epoll' "$WORK/server.log"
  grep -Fq 'epoll input buffer pool: enabled' "$WORK/server.log"
fi

PORT="$PORT" TOKEN="$TOKEN" python3 - <<'PY'
import os
import re
import socket
import sys
import time

port = int(os.environ["PORT"])
token = os.environ["TOKEN"].encode()
clients = []

def read_response(sock, slow):
    data = bytearray()
    header_end = -1
    expected = None
    while expected is None or len(data) < expected:
        try:
            part = sock.recv(96 if slow else 4096)
        except socket.timeout:
            raise RuntimeError("slow-reader response timed out")
        if not part:
            raise RuntimeError("connection closed before response completed")
        data.extend(part)
        if header_end < 0:
            header_end = data.find(b"\r\n\r\n")
            if header_end >= 0:
                header = bytes(data[:header_end])
                match = re.search(rb"(?im)^Content-Length:\s*(\d+)\s*$", header)
                if not match:
                    raise RuntimeError("response omitted Content-Length")
                expected = header_end + 4 + int(match.group(1))
        if slow:
            time.sleep(0.002)
    return bytes(data[:expected])

try:
    for _ in range(8):
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.settimeout(15)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 512)
        s.sendall(
            b"GET /feed HTTP/1.1\r\nHost: local\r\nAuthorization: Bearer "
            + token + b"\r\n\r\n"
        )
        clients.append(s)
    # Let the server fill its send queue before allowing a deliberately slow read.
    time.sleep(0.5)
    for s in clients:
        first = read_response(s, slow=True)
        if not first.startswith(b"HTTP/1.1 200 "):
            raise RuntimeError("feed response was not HTTP 200")
        s.sendall(b"GET /health HTTP/1.1\r\nHost: local\r\n\r\n")
        second = read_response(s, slow=False)
        if not second.startswith(b"HTTP/1.1 200 "):
            raise RuntimeError("keep-alive health response was not HTTP 200")
    print("8 slow feed readers and subsequent keep-alive requests: PASS")
except Exception as exc:
    print(f"slow-reader failure: {exc}", file=sys.stderr)
    sys.exit(1)
finally:
    for s in clients:
        s.close()
PY
