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
# io_uring direct descriptors: accepted sockets live in io_uring's own file
# tables, not the process fd table, so one process holds connections past
# LimitNOFILE (up to 8 rings of HTTP_NSLOT) without front processes.
# HTTP_DIRECT=0 goes back to plain descriptors and HTTP_FRONTS front processes.
export HTTP_DIRECT="${HTTP_DIRECT:-1}"
# Multishot receive into a shared provided-buffer ring (4,096 x 2 KB per ring)
export HTTP_RXMULTI="${HTTP_RXMULTI:-1}"
export HTTP_FRONTS="${HTTP_FRONTS:-3}"
export HTTP_BUFSZ="${HTTP_BUFSZ:-8192}"
# Each event-loop pass waits for 256 completions or 1 ms, whichever comes first,
# so one io_uring_enter serves a batch instead of a single completion.
export HTTP_WAIT_MIN="${HTTP_WAIT_MIN:-256}"
export HTTP_WAIT_US="${HTTP_WAIT_US:-1000}"
exec ./feed_server.x
