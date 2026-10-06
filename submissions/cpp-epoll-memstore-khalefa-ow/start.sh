#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT, plus optional
# STORE_DIR (default: $SQLITE_PATH.memstore), WAL_SYNC (normal, the default, or full), SNAPSHOT_WAL_MB (default 64).
# The first start converts feed.db into the store's snapshot; later starts reuse the store.
set -euo pipefail
DIR="$(dirname "$0")"
"$DIR/bin/import" "$SQLITE_PATH" "${STORE_DIR:-$SQLITE_PATH.memstore}"
exec "$DIR/bin/server"
