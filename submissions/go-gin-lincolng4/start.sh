#!/usr/bin/env bash
# Runs the server in the foreground. Configured only by SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
