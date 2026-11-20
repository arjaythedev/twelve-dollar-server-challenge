#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PORT=${PORT:-3337}
WORK=$(mktemp -d)
SERVER_BIN=${FEED_BIN:-$HERE/bin/feed}
TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["token"])' "$ROOT/seed/tokens.json")
SERVER=""

cleanup() {
  if [[ -n "$SERVER" ]]; then
    kill "$SERVER" 2>/dev/null || true
    wait "$SERVER" 2>/dev/null || true
  fi
  if [[ -n "${FAILURE_SERVER_LOG:-}" && -f "$WORK/server.log" ]]; then
    cp "$WORK/server.log" "$FAILURE_SERVER_LOG"
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

cp "$ROOT/seed/feed.db" "$WORK/feed.db"
python3 - "$WORK/feed.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("""CREATE TRIGGER force_rollback BEFORE INSERT ON posts
              BEGIN SELECT RAISE(ROLLBACK, 'forced transaction failure'); END""")
db.commit()
db.close()
PY

SQLITE_PATH="$WORK/feed.db" JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT="$PORT" FEED_L2_URING=1 FEED_WORKERS=1 \
  "$SERVER_BIN" >"$WORK/server.log" 2>&1 &
SERVER=$!
for _ in $(seq 300); do
  if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; then break; fi
  if ! kill -0 "$SERVER" 2>/dev/null; then cat "$WORK/server.log"; exit 1; fi
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null
grep -Fq 'transport: l2 io_uring' "$WORK/server.log"

PORT="$PORT" TOKEN="$TOKEN" python3 - <<'PY'
import os, socket
port, token = int(os.environ['PORT']), os.environ['TOKEN']
body = b'{"body":"must not be acknowledged"}'
request = (b'POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\n'
           b'Authorization: Bearer ' + token.encode() + b'\r\n'
           b'Content-Type: application/json\r\nContent-Length: ' + str(len(body)).encode() +
           b'\r\nConnection: close\r\n\r\n' + body)
s = socket.create_connection(('127.0.0.1', port), timeout=3)
s.settimeout(5)
s.sendall(request)
response = bytearray()
while True:
    part = s.recv(4096)
    if not part:
        break
    response.extend(part)
s.close()
status = bytes(response).split(b' ', 2)[1]
assert status == b'500', bytes(response)
assert b' 201 ' not in response, bytes(response)
print('write_failure_status=500; provisional_201=absent; failed_connection=closed')
PY

curl -sf "http://127.0.0.1:$PORT/health" | grep -Fq '"status":"ok","db":"ok"'
python3 - "$WORK/feed.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT count(*) FROM posts WHERE body='must not be acknowledged'").fetchone()[0] == 0
print('rolled_back_row_count=0; followup_health=ok')
PY
