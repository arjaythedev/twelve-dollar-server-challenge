#!/usr/bin/env bash
# PGO training: runs the instrumented server on a synthetic database (its own random data,
# none of the seed) and drives the load test's request mix through it with src/train.c.
# Usage: train.sh <instrumented-server-binary>
set -euo pipefail
cd "$(dirname "$0")/.."
SERVER=$1
PORT=${TRAIN_PORT:-3977}
DIR=$(mktemp -d)
trap 'kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true; rm -rf "$DIR"' EXIT
SECRET="train-$RANDOM-$RANDOM"
USERS=500000; POSTS=40000; LIKES=160000
MAKE_TRAIN_DB="$DIR/train.db" TRAIN_USERS=$USERS TRAIN_POSTS=$POSTS TRAIN_LIKES=$LIKES "$SERVER"
${CC:-gcc} -O2 -I src src/train.c -o "$DIR/train"
SQLITE_PATH="$DIR/train.db" JWT_SECRET="$SECRET" HOST=127.0.0.1 PORT=$PORT "$SERVER" > "$DIR/server.log" 2>&1 &
PID=$!
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$PORT/health" > /dev/null && break; sleep 0.2; done
"$DIR/train" 127.0.0.1 $PORT "$SECRET" $USERS $POSTS "${TRAIN_REQUESTS:-150000}" 64
kill -TERM $PID
wait $PID 2>/dev/null || true
ls pgo/*.gcda > /dev/null 2>&1 || ls pgo/*/*.gcda > /dev/null 2>&1 || { echo "no profile written" >&2; exit 1; }
