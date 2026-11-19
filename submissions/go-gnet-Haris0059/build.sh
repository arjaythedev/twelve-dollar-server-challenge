#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Pinned SQLite source, checksum-verified; compiled into the binary by cgo (sqlite_amalg.c).
SQLITE=sqlite-autoconf-3530400
SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
if [ ! -f sqlite/sqlite3.c ]; then
  tmp="$(mktemp -d)"
  curl -fsSLo "$tmp/s.tar.gz" "https://www.sqlite.org/2026/$SQLITE.tar.gz"
  echo "$SHA256  $tmp/s.tar.gz" | sha256sum -c - >/dev/null
  mkdir -p sqlite
  tar -xzf "$tmp/s.tar.gz" -C sqlite --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  rm -rf "$tmp"
fi
CGO_ENABLED=1 /usr/local/go/bin/go build -trimpath -o bin/server .
