#!/usr/bin/env bash
# Runs the server in the foreground. Configuration comes from the environment
# variables in SPEC.md: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
