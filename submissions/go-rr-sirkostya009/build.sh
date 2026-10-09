#!/usr/bin/env bash
# Builds the server as a normal user. Dependencies are pinned in go.mod/go.sum;
# the generated router and codecs (*_gen.go, *_ggen.go) are committed, so the
# build only compiles. cgo links Ubuntu's libsqlite3 (install.sh).
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH" GOTOOLCHAIN=local CGO_ENABLED=1
go build -trimpath -ldflags='-s -w' -o bin/server ./cmd/server
