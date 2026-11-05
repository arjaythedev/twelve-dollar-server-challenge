#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# Optional: BTREE=0 to force SQL reads; SERVER_IO=epoll to skip io_uring on Linux.
set -euo pipefail
cd "$(dirname "$0")"
export GOMAXPROCS="${GOMAXPROCS:-1}"
export GOMEMLIMIT="${GOMEMLIMIT:-768MiB}"
exec ./bin/server
