#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

export CGO_ENABLED=1
export GOMAXPROCS=1
exec ./bin/server
