#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# The scoring harness grants the service LimitNOFILE=65535 (SPEC); rule 11 makes
# settings inside our own process fair game, so a plain-shell start mirrors that
# grant instead of running into the 1024 default.
ulimit -n 65535 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

# One background thread: the only threadpool consumer in this app is the sqlite3
# rw write path (no httpc, no fs, no DNS), already serialized by the connection
# mutex — the pool's spare threads could never be used.
export UV_THREADPOOL_SIZE=1

exec ./.lunet/lunet-run server.lua
