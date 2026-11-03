#!/usr/bin/env bash
# start.sh: runs the server in the foreground
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"

export HOST="${HOST:-127.0.0.1}"
export PORT="${PORT:-3000}"
export SQLITE_PATH="${SQLITE_PATH:-$(pwd)/seed/feed.db}"
export JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}"
printf -v SERVER_START_TIME "%(%s)T" -1
export SERVER_START_TIME

# Ensure SQLite database is in WAL mode and optimized before accepting connections
sqlite3 "$SQLITE_PATH" "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA optimize;" 2>/dev/null || true

exec socat -b65536 TCP-LISTEN:"$PORT",bind="$HOST",fork,reuseaddr,tcp-nodelay,backlog=2048 EXEC:"$DIR/worker.sh",pipes
