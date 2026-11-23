#!/usr/bin/env bash
# Generates a synthetic training database and drives realistic pipe=1 PGO traffic across 2 rings.
set -euo pipefail
SERVER=${1:?server binary required}
DRIVER=${2:?driver binary required}

TMP=$(mktemp -d)
trap 'kill "${SPID:-}" 2>/dev/null || true; wait "${SPID:-}" 2>/dev/null || true; rm -rf "$TMP"' EXIT

DB="$TMP/train.db"
SECRET="pgo-train-secret"
PORT=$((28000 + ($$ % 10000)))

"$SERVER" --make-train-db "$DB"

SQLITE_PATH="$DB" JWT_SECRET="$SECRET" HOST="127.0.0.1" PORT="$PORT" \
  CACHE_KIB=32768 RINGS=2 RING_SLOTS=1024 RECV_BUFS=512 CKPT_PASS=256 WRITE_WINDOW_US=5000 \
  "$SERVER" >/dev/null 2>&1 &
SPID=$!

for _ in $(seq 1 100); do
  if (echo >/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
  sleep 0.05
done

"$DRIVER" "$PORT" "$SECRET" "${TRAIN_REQS:-60000}"

kill -TERM "$SPID"
wait "$SPID"
SPID=""
