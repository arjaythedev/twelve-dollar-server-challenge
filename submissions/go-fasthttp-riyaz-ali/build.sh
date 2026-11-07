#!/usr/bin/env bash

# Builds the server as a normal user. Dependencies are pinned in go.sum.
set -euo pipefail

cd "$(dirname "$0")"
GO=/usr/local/go/bin/go
CGO_CFLAGS="-O3 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_MAX_EXPR_DEPTH=0"

[ -x "$GO" ] || GO=go
CGO_CFLAGS=$CGO_CFLAGS CGO_ENABLED=1 "$GO" build -trimpath -o bin/server .