#!/usr/bin/env bash
# Builds the app as a normal user. Downloads the pinned SQLite amalgamation (checksum-verified),
# which cgo compiles into the binary together with fastdb.c.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_ZIP=sqlite-amalgamation-3540000.zip
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
mkdir -p .sqlite-download
if [ ! -f ".sqlite-download/$SQLITE_ZIP" ]; then
  curl -fsSL -o ".sqlite-download/$SQLITE_ZIP.part" "https://www.sqlite.org/2026/$SQLITE_ZIP"
  mv ".sqlite-download/$SQLITE_ZIP.part" ".sqlite-download/$SQLITE_ZIP"
fi
echo "$SQLITE_SHA256  .sqlite-download/$SQLITE_ZIP" | sha256sum -c -
unzip -o -j -q ".sqlite-download/$SQLITE_ZIP" '*/sqlite3.c' '*/sqlite3.h' -d .

export PATH="/usr/local/go/bin:$PATH" GOTOOLCHAIN=local CGO_ENABLED=1
go build -trimpath -ldflags='-s -w' -o bin/server .
