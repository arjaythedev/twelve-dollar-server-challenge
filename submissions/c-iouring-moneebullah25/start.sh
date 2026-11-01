#!/usr/bin/env bash
# Runs the server in the foreground. Configuration comes only from the SPEC
# environment variables: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
