#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/uvicorn main:app \
  --host "${HOST:-127.0.0.1}" \
  --port "${PORT:-3000}" \
  --loop uvloop \
  --http httptools \
  --no-access-log \
  --timeout-keep-alive 75
