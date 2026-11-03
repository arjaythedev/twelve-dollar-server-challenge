#!/usr/bin/env bash
# start.sh — runs the server in the foreground. Configured only by SPEC env vars
# (SQLITE_PATH, JWT_SECRET, HOST, PORT). We serve the internet DIRECTLY (no
# Nginx): the benchmark sets HOST=0.0.0.0, PORT=80 and the server binds those.
set -euo pipefail
cd "$(dirname "$0")"

# 1 vCPU: keep the Go scheduler lean. One extra P can help overlap cgo/SQLite
# blocking calls with network work without oversubscribing the single core.
export GOMAXPROCS="${GOMAXPROCS:-2}"
# Reader connections in the pool (WAL concurrent readers).
export READERS="${READERS:-4}"
# Soft heap limit: makes the GC reclaim more aggressively as RSS approaches the
# box's 2 GB, trading a little CPU for lower peak memory. Leaves headroom for
# non-heap use (goroutine stacks, cgo/SQLite, mmap page cache). Tunable.
export GOMEMLIMIT="${GOMEMLIMIT:-1600MiB}"

exec ./bin/server
