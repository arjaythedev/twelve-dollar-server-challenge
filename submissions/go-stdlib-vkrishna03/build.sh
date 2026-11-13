#!/usr/bin/env bash
# Builds the server as a normal user. No third-party Go modules: the Go standard library, plus
# SQLite 3.53.4 compiled in through cgo from the official amalgamation (pinned by sha256).
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-autoconf-3530400
SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
if [ ! -f sqlite/sqlite3.c ]; then
  tmp="$(mktemp -d)"
  curl -fsSLo "$tmp/sqlite.tar.gz" "https://www.sqlite.org/2026/$SQLITE.tar.gz"
  echo "$SHA256  $tmp/sqlite.tar.gz" | sha256sum -c - >/dev/null
  mkdir -p sqlite
  tar -xzf "$tmp/sqlite.tar.gz" -C sqlite --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  rm -r "$tmp"
fi
export GOTOOLCHAIN=local CGO_ENABLED=1
/usr/local/go/bin/go build -trimpath -ldflags='-s -w' -o bin/server .
