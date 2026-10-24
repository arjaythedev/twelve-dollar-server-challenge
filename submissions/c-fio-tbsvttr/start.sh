#!/usr/bin/env bash
set -euo pipefail
ulimit -Sn "$(ulimit -Hn)"
exec "$(dirname "$0")/bin/server"
