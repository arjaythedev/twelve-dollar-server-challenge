#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
shards build server --production --release --no-debug
