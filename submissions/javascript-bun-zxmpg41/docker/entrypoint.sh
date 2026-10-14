#!/usr/bin/env bash
# Every start gets a fresh copy of the seed database (as on the benchmark box).
set -euo pipefail
[ -f /seed/feed.db ] || { echo "mount seed/ at /seed (run: bash seed/make-seed.sh)"; exit 1; }
rm -f "$SQLITE_PATH" "$SQLITE_PATH-wal" "$SQLITE_PATH-shm"
cp /seed/feed.db "$SQLITE_PATH"
exec bash /app/start.sh
