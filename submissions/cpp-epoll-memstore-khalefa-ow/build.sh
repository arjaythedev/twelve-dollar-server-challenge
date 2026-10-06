#!/usr/bin/env bash
# Builds bin/server (no SQLite) and bin/import (the one-time feed.db -> snapshot converter, the only
# part that links SQLite). Downloads the pinned SQLite amalgamation, checksum-verified.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VERSION=3530400   # SQLite 3.53.4
SQLITE_YEAR=2026
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c

mkdir -p vendor bin
if [ ! -f vendor/sqlite3.c ] || [ "$(cat vendor/VERSION 2>/dev/null)" != "$SQLITE_VERSION" ]; then
  tarball="vendor/sqlite-autoconf-$SQLITE_VERSION.tar.gz"
  curl -fsSL -o "$tarball" "https://www.sqlite.org/$SQLITE_YEAR/sqlite-autoconf-$SQLITE_VERSION.tar.gz"
  echo "$SQLITE_SHA256  $tarball" | sha256sum -c --quiet
  tar -xzf "$tarball" -C vendor --strip-components=1 \
    "sqlite-autoconf-$SQLITE_VERSION/sqlite3.c" "sqlite-autoconf-$SQLITE_VERSION/sqlite3.h"
  rm -f "$tarball"
  echo "$SQLITE_VERSION" > vendor/VERSION
fi

OPT=(-O2 -march=native -fno-plt)   # built on the box it runs on, so -march=native is safe
CXXFLAGS=(-std=c++17 -Wall -Wextra -Wno-deprecated-declarations)

[ vendor/sqlite3.o -nt vendor/sqlite3.c ] ||
  gcc -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -c vendor/sqlite3.c -o vendor/sqlite3.o
g++ "${OPT[@]}" "${CXXFLAGS[@]}" -Ivendor src/import.cpp vendor/sqlite3.o -o bin/import
g++ "${OPT[@]}" "${CXXFLAGS[@]}" src/server.cpp -o bin/server -lcrypto -pthread
echo "built $(pwd)/bin/server and bin/import"
