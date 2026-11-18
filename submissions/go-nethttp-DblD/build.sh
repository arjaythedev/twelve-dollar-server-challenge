#!/usr/bin/env bash
# Runs as a normal user. Dependencies are pinned in go.mod and verified against go.sum.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH" CGO_ENABLED=1
go mod download
go mod verify
go build -mod=readonly -trimpath -ldflags='-s -w' -o bin/server .
