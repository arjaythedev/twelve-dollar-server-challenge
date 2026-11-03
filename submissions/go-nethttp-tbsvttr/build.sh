#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./sqlite-build.sh
export PATH="/opt/twelve-go-1.27.1/bin:/usr/local/go/bin:$PATH" CGO_ENABLED=1 GOTOOLCHAIN=local
go mod download
go mod verify
go build -mod=readonly -tags 'libsqlite3 sqlite_omit_load_extension' -trimpath -ldflags='-s -w' -o bin/server .
