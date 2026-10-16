#!/usr/bin/env bash
# Builds as a normal user: fetches the pinned SQLite amalgamation (sha256-checked) and compiles
# it into the Go binary through cgo.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE_ZIP=sqlite-amalgamation-3540000
SQLITE_URL="https://www.sqlite.org/2026/${SQLITE_ZIP}.zip"
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d

if [ ! -f sqlite/.version ] || [ "$(cat sqlite/.version)" != "$SQLITE_SHA256" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL "$SQLITE_URL" -o "$tmp/sqlite.zip"
  echo "${SQLITE_SHA256}  $tmp/sqlite.zip" | sha256sum -c -
  unzip -q -j "$tmp/sqlite.zip" "${SQLITE_ZIP}/sqlite3.c" "${SQLITE_ZIP}/sqlite3.h" -d "$tmp/out"
  rm -rf sqlite && mkdir sqlite
  mv "$tmp/out/sqlite3.c" "$tmp/out/sqlite3.h" sqlite/
  echo "$SQLITE_SHA256" > sqlite/.version
  rm -rf "$tmp"
fi

export CGO_ENABLED=1 GOTOOLCHAIN=local GOFLAGS=-mod=mod
/usr/local/go/bin/go build -trimpath -ldflags='-s -w' -o bin/server .
