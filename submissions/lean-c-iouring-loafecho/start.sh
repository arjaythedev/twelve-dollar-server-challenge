#!/usr/bin/env bash
# start.sh — runs the server in the foreground. Config: SQLITE_PATH,
# JWT_SECRET, HOST (default 0.0.0.0), PORT (default 80).
cd "$(dirname "$0")"
exec ./bin/server
