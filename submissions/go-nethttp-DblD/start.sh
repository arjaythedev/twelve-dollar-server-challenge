#!/usr/bin/env bash
# Foreground; configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
