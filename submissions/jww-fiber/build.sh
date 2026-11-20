#!/usr/bin/env bash
# Builds the server as a normal user. Dependencies are pinned in go.mod/go.sum.
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/usr/local/go/bin:/usr/local/bin:$PATH"
export CGO_ENABLED=1
export GOTOOLCHAIN=local

go mod download
go mod verify
mkdir -p bin
go build -mod=readonly -trimpath -tags 'sqlite_omit_load_extension' \
  -ldflags='-s -w' -o bin/server .
