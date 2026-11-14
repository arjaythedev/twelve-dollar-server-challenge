#!/usr/bin/env bash
# Runs the server in the foreground. Configuration comes from SQLITE_PATH,
# JWT_SECRET, HOST and PORT (see SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
exec ./bin/server
