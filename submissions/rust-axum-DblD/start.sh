#!/usr/bin/env bash
# Foreground; configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec ./target/release/feed-server
