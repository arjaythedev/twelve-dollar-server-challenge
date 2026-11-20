#!/usr/bin/env bash
# Protocol and durability checks the 42-check suite does not cover:
#   * a pipelined GET + POST + GET in one write, answered in order
#   * a request split across two writes
#   * Connection: close
#   * read-your-writes on the next request
#   * kill -9 with writes in flight: every acknowledged 201 must survive
#
#   bash tests/protocol.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PORT=${PORT:-3321}
POST_MODE=${POST_MODE:-0}
WORK=$(mktemp -d)
SERVER_BIN=${FEED_BIN:-$HERE/bin/feed}
TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["token"])' "$ROOT/seed/tokens.json")
SERVER=""

cleanup() {
  if [ -n "$SERVER" ]; then
    pkill -P "$SERVER" 2>/dev/null || true
    kill "$SERVER" 2>/dev/null || true
    wait "$SERVER" 2>/dev/null || true
  fi
  if [[ -n "${PROTOCOL_SERVER_LOG:-}" && -f "$WORK/log" ]]; then
    cp "$WORK/log" "$PROTOCOL_SERVER_LOG"
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

start() {
  : >>"$WORK/log"
  local first_line=$(( $(wc -l < "$WORK/log") + 1 ))
  if [[ "${PROTOCOL_TRANSPORT:-epoll}" == l2 ]]; then
    SQLITE_PATH="$WORK/feed.db" JWT_SECRET=twelve-dollar-challenge \
      HOST=127.0.0.1 PORT="$PORT" FEED_L2_URING=1 FEED_WORKERS=1 \
      FEED_FORCE_SQL_POST="$POST_MODE" "$SERVER_BIN" >>"$WORK/log" 2>&1 &
  else
    SQLITE_PATH="$WORK/feed.db" JWT_SECRET=twelve-dollar-challenge \
      HOST=127.0.0.1 PORT="$PORT" FEED_EPOLL=1 \
      FEED_EPOLL_INPUT_POOL="${FEED_EPOLL_INPUT_POOL:-0}" \
      FEED_WORKERS="${FEED_WORKERS:-1}" FEED_FORCE_SQL=0 \
      FEED_FORCE_SQL_POST="$POST_MODE" "$SERVER_BIN" >>"$WORK/log" 2>&1 &
  fi
  SERVER=$!
  for _ in $(seq 300); do
    if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; then
      if [[ -n "${PROTOCOL_REQUIRE_TRANSPORT:-}" ]]; then
        tail -n +"$first_line" "$WORK/log" | grep -Fq "transport: $PROTOCOL_REQUIRE_TRANSPORT" || { cat "$WORK/log"; return 1; }
      fi
      return 0
    fi
    state=$(ps -o stat= -p "$SERVER" 2>/dev/null || true)
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait "$SERVER" 2>/dev/null || true
      SERVER=""
      echo "server exited before becoming healthy"; cat "$WORK/log"; return 1
    fi
    sleep 0.1
  done
  echo "server never became healthy"; cat "$WORK/log"; return 1
}

stop() {
  pkill -9 -P "$SERVER" 2>/dev/null || true
  kill -9 "$SERVER" 2>/dev/null || true
  wait "$SERVER" 2>/dev/null || true
  SERVER=""
}

cp "$ROOT/seed/feed.db" "$WORK/feed.db"
start
grep -q "read plans: enabled" "$WORK/log" || {
  echo "the plan did not activate; the rest proves nothing"; cat "$WORK/log"; exit 1; }
if [[ "${PROTOCOL_TRANSPORT:-epoll}" == l2 ]]; then
  grep -q "transport: l2 io_uring" "$WORK/log" || { cat "$WORK/log"; exit 1; }
else
  if [[ "${FEED_EPOLL_INPUT_POOL:-0}" == 1 ]]; then pool_state=enabled; else pool_state=disabled; fi
  grep -Fq "epoll input buffer pool: $pool_state" "$WORK/log" || { cat "$WORK/log"; exit 1; }
fi

PORT="$PORT" TOKEN="$TOKEN" python3 - <<'PY'
import os, socket, sys
port, token = int(os.environ["PORT"]), os.environ["TOKEN"]
fail = []

def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(3); return s

def drain(s):
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    return buf

def statuses(buf):
    out = []
    for chunk in buf.split(b"HTTP/1.1 ")[1:]:
        out.append(int(chunk[:3]))
    return out

# 1. pipelined GET + POST + GET in one write, answered in order
body = b'{"body":"pipelined probe"}'
req = (b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n"
       b"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token.encode() +
       b"\r\nContent-Type: application/json\r\nContent-Length: " + str(len(body)).encode() +
       b"\r\n\r\n" + body +
       b"GET /posts/2 HTTP/1.1\r\nHost: x\r\n\r\n")
s = conn(); s.sendall(req); buf = drain(s); s.close()
st = statuses(buf)
if st != [200, 201, 200]:
    fail.append("pipelined order/number: %r" % (st,))
print("pipelined GET+POST+GET in one write:", st, "PASS" if not fail else "FAIL")

# 2. the write is visible to a later request
s = conn(); s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n"); feed = drain(s); s.close()
if b'"body":"pipelined probe"' not in feed.split(b'"posts":[')[1][:400]:
    fail.append("created post is not at the top of /feed")
print("created post leads /feed:", b'"body":"pipelined probe"' in feed.split(b'"posts":[')[1][:400])

# 3. a request split across two writes
s = conn(); s.sendall(b"GET /posts/3 HTTP/1.1\r\nHost: x\r\n")
import time; time.sleep(0.05); s.sendall(b"\r\n")
r = drain(s); s.close()
if statuses(r) != [200]:
    fail.append("split request: %r" % (statuses(r),))
print("request split across two writes:", statuses(r))

# 3b. Drop a partially buffered request, then accept a new connection.
s = conn(); s.sendall(b"GET /posts/"); time.sleep(0.05); s.close()
s = conn(); s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
r = drain(s); s.close()
if statuses(r) != [200]:
    fail.append("request after abandoned partial input: %r" % (statuses(r),))
print("request after abandoned partial input:", statuses(r))

# 4. Connection: close closes the socket
s = conn()
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
r = drain(s)
tail = s.recv(64)
s.close()
if statuses(r) != [200] or tail != b"":
    fail.append("Connection: close: statuses=%r tail=%r" % (statuses(r), tail))
print("Connection: close:", statuses(r), "socket closed:", tail == b"")

sys.exit(1 if fail else 0)
PY

# 5. kill -9 with writes in flight: every acknowledged 201 must survive
rm -f "$WORK"/codes "$WORK"/resp.*
pids=()
for i in $(seq 40); do
  curl -s -X POST "http://127.0.0.1:$PORT/posts" -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' -d "{\"body\":\"durable $i\"}" \
    -o "$WORK/resp.$i" -w "%{http_code}\n" >>"$WORK/codes" &
  pids+=($!)
done
# Wait for the clients only: a bare `wait` would also wait for the server job.
wait "${pids[@]}"
ack=$(WORK="$WORK" python3 - <<'PY'
import glob, json, os
ids = []
for path in glob.glob(os.path.join(os.environ["WORK"], "resp.*")):
    try:
        with open(path, "r", encoding="utf-8") as f:
            ids.append(int(json.load(f)["post"]["id"]))
    except (OSError, ValueError, KeyError, TypeError):
        pass
print("\n".join(map(str, sorted(ids))))
PY
)
stop
if [[ -n "${PROTOCOL_RESTART_PORT:-}" ]]; then
  PORT="$PROTOCOL_RESTART_PORT"
  echo "durability restart uses explicit fresh port: $PORT"
fi
start
missing=0
for id in $ack; do
  code=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/posts/$id")
  [ "$code" = "200" ] || { echo "missing after kill -9: post $id (HTTP $code)"; missing=$((missing + 1)); }
done
echo "acknowledged 201s: $(echo "$ack" | wc -w), lost after kill -9: $missing"
[ "$missing" = 0 ] || exit 1

grep -q "read plans disabled" "$WORK/log" && {
  echo "the plan disabled itself during the run"; exit 1; }
echo "protocol: ok"
