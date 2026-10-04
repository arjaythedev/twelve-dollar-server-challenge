#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads the pinned SQLite amalgamation (checksum-verified)
# and compiles it into the binary with the server, so its compile-time options are ours.
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

# Built on the box it runs on, so -march=native is safe.
OPT=(-O2 -march=native -flto=auto -fno-plt)
SQLITE_FLAGS=(
  -DSQLITE_THREADSAFE=2              # each connection is used by one thread only
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_USE_ALLOCA
)

# Compiled as C through the g++ driver so both objects share one GCC version (LTO needs that).
g++ -x c "${OPT[@]}" "${SQLITE_FLAGS[@]}" -c vendor/sqlite3.c -o vendor/sqlite3.o
g++ "${OPT[@]}" -std=c++17 -Wall -Wextra -Wno-deprecated-declarations -Ivendor \
  src/main.cpp vendor/sqlite3.o -o bin/server -lcrypto -lpthread
echo "built $(pwd)/bin/server"
