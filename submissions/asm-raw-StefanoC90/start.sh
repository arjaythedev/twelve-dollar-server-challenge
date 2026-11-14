#!/usr/bin/env bash
# Runs the server in the foreground. Config comes only from SQLITE_PATH,
# JWT_SECRET, HOST and PORT (see SPEC.md).
set -euo pipefail
exec "$(dirname "$0")/bin/server"
