#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# The server itself defaults to GOMAXPROCS=2.
set -euo pipefail
cd "$(dirname "$0")"
exec ./bin/server
