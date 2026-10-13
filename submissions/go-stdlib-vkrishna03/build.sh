#!/usr/bin/env bash
# Builds the server as a normal user. No third-party modules: the Go standard library, plus cgo
# against the system SQLite from install.sh.
set -euo pipefail
cd "$(dirname "$0")"
export GOTOOLCHAIN=local CGO_ENABLED=1
/usr/local/go/bin/go build -trimpath -ldflags='-s -w' -o bin/server .
