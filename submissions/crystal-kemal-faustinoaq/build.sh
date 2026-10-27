#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
shards install --frozen --without-development
crystal build --release --no-debug --mcpu native src/server.cr -o bin/server
