#!/usr/bin/env bash
set -euo pipefail
export MUX_LAYOUT="${MUX_LAYOUT:-inline}"
exec "$(dirname "$0")/bin/server"
