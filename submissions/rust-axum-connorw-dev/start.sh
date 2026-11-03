#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true # one descriptor per keep-alive client
exec ./target/release/feed-server
