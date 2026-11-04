#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
: "${SQLITE_PATH:?SQLITE_PATH is required}"
export JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}"
export HOST="${HOST:-127.0.0.1}"
export PORT="${PORT:-3000}"
# Maximum open connections, one per user. The official box runs with
# LimitNOFILE=65535, and the server keeps 7 descriptors of its own, so 65520
# uses almost all of them while leaving room to accept and close a connection
# when the table is full. Raise it only together with the fd limit.
export HTTP_NSLOT="${HTTP_NSLOT:-65520}"
# Front processes, each with its own descriptor limit and HTTP_NSLOT more
# connections; requests reach the one database process through shared memory.
# Three fronts and the database process hold up to 262,080 connections.
export HTTP_FRONTS="${HTTP_FRONTS:-3}"
export HTTP_BUFSZ="${HTTP_BUFSZ:-8192}"
exec ./feed_server.x
