#!/usr/bin/env bash
# PGO training (build time only): a synthetic database, the instrumented server, the training
# client, then a clean exit so the profile is written. Usage: train.sh <server binary> <train binary>
set -euo pipefail
SERVER="$1"
TRAIN="$2"
DIR="$(mktemp -d)"
trap 'kill "${PID:-}" 2>/dev/null || true; rm -rf "$DIR"' EXIT
"$SERVER" --make-train-db "$DIR/train.db"
PORT=$((20000 + RANDOM % 20000))
SQLITE_PATH="$DIR/train.db" JWT_SECRET=training-secret HOST=127.0.0.1 PORT=$PORT "$SERVER" > "$DIR/server.log" 2>&1 &
PID=$!
for _ in $(seq 100); do
  curl -sf "http://127.0.0.1:$PORT/health" > /dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" > /dev/null || { echo "training server did not start"; cat "$DIR/server.log"; exit 1; }
rc=0
timeout 300 "$TRAIN" 127.0.0.1 "$PORT" training-secret "${TRAIN_REQUESTS:-60000}" || rc=$?
if [ "$rc" != 0 ]; then
  echo "training client failed (rc=$rc); server log:" >&2
  tail -20 "$DIR/server.log" >&2 || true
  exit 1
fi
kill -TERM "$PID"
for _ in $(seq 100); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.1
done
kill -KILL "$PID" 2>/dev/null || true
wait "$PID" || true
PID=""
tail -3 "$DIR/server.log" || true
