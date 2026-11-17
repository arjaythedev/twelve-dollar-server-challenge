#!/usr/bin/env bash
# Runs as a normal user. The only dependency is SQLite itself: the official amalgamation,
# pinned by version and sha256. It goes in sqlite/ and db.c includes it, so cgo compiles it
# once, as part of db.c.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH" CGO_ENABLED=1

SQLITE=sqlite-amalgamation-3530400
SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
if [ ! -f sqlite/sqlite3.c ]; then
  mkdir -p sqlite
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/$SQLITE.zip" -o sqlite.zip
  printf '%s  %s\n' "$SHA256" sqlite.zip | sha256sum -c -
  unzip -q -o -j sqlite.zip "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h" -d sqlite
  rm -f sqlite.zip
fi
go build -trimpath -ldflags='-s -w' -o bin/server .
