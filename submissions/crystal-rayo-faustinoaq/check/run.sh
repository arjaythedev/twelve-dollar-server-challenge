#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export SQLITE_PATH=$PWD/build/integration.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=${PORT:-18374}
cp "${SEED_DIR:?set SEED_DIR}/feed.db" "$SQLITE_PATH"
bash start.sh > build/integration.log 2>&1 &
PID=$!
cleanup() {
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  rm -f "$SQLITE_PATH" "$SQLITE_PATH-wal" "$SQLITE_PATH-shm"
}
trap cleanup EXIT
for _ in $(seq 60); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.5; done
python3 check/integration.py
