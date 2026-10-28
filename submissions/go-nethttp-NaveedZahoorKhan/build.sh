#!/usr/bin/env bash
# build.sh — runs as a normal user. Builds a static-ish cgo binary.
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/usr/local/go/bin:$PATH"
export CGO_ENABLED=1

# Build the embedded SQLite (from go-sqlite3's vendored amalgamation) with
# performance flags. These are compile-time options for our own binary, not
# box tuning. We pass them via CGO_CFLAGS.
# SQLITE_THREADSAFE is set by the vendored driver's cgo preamble (=2,
# multi-thread: safe because every connection is owned by exactly one goroutine
# — the reader pool, the single writer, and the health conn). We do NOT redefine
# it here to avoid a redefinition conflict. _mutex=no in the DSN then drops the
# per-connection mutex entirely.
export CGO_CFLAGS="-O2 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
  -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_USE_ALLOCA \
  -DSQLITE_ENABLE_STAT4"

go build -mod=vendor -tags "sqlite_omit_load_extension" \
  -ldflags "-s -w" -o bin/server .

echo "built bin/server"
