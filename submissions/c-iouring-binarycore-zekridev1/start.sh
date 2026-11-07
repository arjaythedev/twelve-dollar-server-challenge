#!/usr/bin/env bash
# start.sh: runs the server in the foreground.
# Configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
