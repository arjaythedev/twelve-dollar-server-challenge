#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
shards install --frozen --without-development
# Use a fixed CPU baseline: native CPU detection produced SIGILL on some CI runners.
crystal build --release --no-debug --mcpu x86-64-v3 src/server.cr -o bin/server
