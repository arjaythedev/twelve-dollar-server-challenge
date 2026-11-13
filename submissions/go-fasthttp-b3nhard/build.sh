#!/usr/bin/env bash
# Builds the server as a normal user. Downloads the pinned Go module from the
# module proxy; no prebuilt binaries are used.
set -euo pipefail
cd "$(dirname "$0")"

GO=go
command -v go >/dev/null 2>&1 || GO=/usr/local/go/bin/go

export CGO_ENABLED=1
"$GO" build -trimpath -ldflags "-s -w" -o bin/server .
