#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
GO=/opt/twelve-go-1.27.1/bin/go
[ -x "$GO" ] || GO=/usr/local/go/bin/go
[ -x "$GO" ] || GO=go
export CGO_ENABLED=1 GOTOOLCHAIN=local
"$GO" build -mod=readonly -trimpath -ldflags='-s -w' -o bin/server .
