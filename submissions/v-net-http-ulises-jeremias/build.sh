#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
VJOBS=1 v -prod -o bin/server server.v
