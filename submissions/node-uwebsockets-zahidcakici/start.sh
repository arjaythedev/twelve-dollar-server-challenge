#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec /opt/node-v24.21.0/bin/node --max-semi-space-size=64 server.js
