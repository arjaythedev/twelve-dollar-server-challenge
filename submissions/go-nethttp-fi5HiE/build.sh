#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH" CGO_ENABLED=1 GOTOOLCHAIN=local CGO_CFLAGS="-O2 -g -DSQLITE_DEFAULT_MEMSTATUS=0"
go build -mod=readonly -trimpath -tags sqlite_omit_load_extension -o bin/server .
