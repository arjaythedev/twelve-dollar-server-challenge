#!/usr/bin/env bash
# Builds bin/server as a normal user: downloads the pinned SQLite amalgamation and liburing source, checks their
# SHA-256, builds liburing's static library, and runs make. On Nix, use `nix build` instead (flake.nix).
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=deps.env
source deps.env

if [ ! -f .deps/sqlite3.c ]; then
  mkdir -p .deps
  curl -fsSL --retry 3 "https://www.sqlite.org/$SQLITE_YEAR/sqlite-amalgamation-$SQLITE_VERSION.zip" -o .deps/sqlite.zip
  echo "$SQLITE_SHA256  .deps/sqlite.zip" | sha256sum -c -
  unzip -q -o -j .deps/sqlite.zip "sqlite-amalgamation-$SQLITE_VERSION/sqlite3.c" "sqlite-amalgamation-$SQLITE_VERSION/sqlite3.h" -d .deps
fi
if [ ! -f .deps/liburing/src/liburing.a ]; then
  mkdir -p .deps/liburing
  curl -fsSL --retry 3 "https://github.com/axboe/liburing/archive/refs/tags/liburing-$LIBURING_VERSION.tar.gz" -o .deps/liburing.tar.gz
  echo "$LIBURING_SHA256  .deps/liburing.tar.gz" | sha256sum -c -
  tar -xzf .deps/liburing.tar.gz -C .deps/liburing --strip-components=1
  (cd .deps/liburing && ./configure --cc="${CC:-gcc}" >/dev/null && make -s -C src -j"$(nproc)" liburing.a >/dev/null)
fi
make -j"$(nproc)" SQLITE_DIR=.deps
