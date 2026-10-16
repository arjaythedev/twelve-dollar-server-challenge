#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
: "${SQLITE_PATH:?SQLITE_PATH is required}"
export JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}"
export HOST="${HOST:-127.0.0.1}"
export PORT="${PORT:-3000}"
# Must be at least the number of expected open connections. Default 32768 handles up
# to ~15k users. For 120k+ concurrent users, set HTTP_NSLOT=131072 or higher.
export HTTP_NSLOT="${HTTP_NSLOT:-32768}"
export HTTP_BUFSZ="${HTTP_BUFSZ:-8192}"
exec ./feed_server.x
