#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

GO_BIN="go"
if [ -x "/usr/local/go/bin/go" ]; then
    GO_BIN="/usr/local/go/bin/go"
fi

export CGO_ENABLED=1
export CGO_CFLAGS="-O3 -DNDEBUG"
mkdir -p bin
"$GO_BIN" build -trimpath -ldflags="-s -w" -o bin/server .
