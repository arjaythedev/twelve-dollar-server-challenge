#!/usr/bin/env bash
set -euo pipefail
export GOMEMLIMIT="${GOMEMLIMIT:-1500MiB}"
exec "$(dirname "$0")/bin/server"
