#!/usr/bin/env bash
# Runs the server in the foreground
set -euo pipefail
exec "$(dirname "$0")/bin/server"
