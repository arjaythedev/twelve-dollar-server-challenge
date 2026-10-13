#!/usr/bin/env bash
# Runs the server in the foreground. Configured entirely by SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec ./build/feedapi
