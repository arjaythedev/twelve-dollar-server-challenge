#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH"
export GOTOOLCHAIN=local
export CGO_ENABLED=1
mkdir -p bin
go build -mod=readonly -trimpath -ldflags='-s -w' -o bin/server .
