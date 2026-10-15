#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
CGO_ENABLED=1 go build -tags libsqlite3 -trimpath -ldflags='-s -w' -o bin/server .
