#!/usr/bin/env bash
# Container entrypoint: starts the server on a fresh copy of the seed database, like the benchmark box.
set -euo pipefail
export SQLITE_PATH=/tmp/db/feed.db JWT_SECRET=twelve-dollar-challenge HOST="${HOST:-0.0.0.0}" PORT="${PORT:-80}"
mkdir -p /tmp/db
cp /seed/feed.db "$SQLITE_PATH"
exec bash /app/start.sh
