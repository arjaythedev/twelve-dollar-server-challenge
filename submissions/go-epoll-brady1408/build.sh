#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads the pinned SQLite amalgamation on first run.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE_ZIP_URL=https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
SQLITE_ZIP_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
mkdir -p .deps bin
if [[ ! -f internal/sqlite/sqlite3.c ]]; then
  curl -fsSL --retry 3 "$SQLITE_ZIP_URL" -o .deps/sqlite.zip
  echo "$SQLITE_ZIP_SHA256  .deps/sqlite.zip" | sha256sum -c -
  unzip -oq .deps/sqlite.zip -d .deps
  cp .deps/sqlite-amalgamation-3540000/sqlite3.c .deps/sqlite-amalgamation-3540000/sqlite3.h internal/sqlite/
fi
export PATH="/usr/local/go/bin:$PATH" CGO_ENABLED=1
go mod download
go build -trimpath -ldflags='-s -w' -o bin/server .
