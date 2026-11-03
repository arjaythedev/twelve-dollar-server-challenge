#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads the pinned SQLite amalgamation
# (SHA-256 verified), compiles it with size/speed trims, then builds the server
# with PGO (train on a synthetic DB) and LTO. No -march=native: the same binary
# runs anywhere (portability over a few % single-machine speed).
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VER=3530400
SQLITE_URL="https://www.sqlite.org/2026/sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c

mkdir -p deps bin build
fetch() { # fetch <url> <sha256> <file>
  if [ ! -f "deps/$3" ] || ! echo "$2  deps/$3" | sha256sum -c --quiet - 2>/dev/null; then
    curl -fsSL --retry 3 -o "deps/$3" "$1"
    echo "$2  deps/$3" | sha256sum -c --quiet -
  fi
}
fetch "$SQLITE_URL" "$SQLITE_SHA256" sqlite.tar.gz
rm -rf deps/sqlite && mkdir -p deps/sqlite
tar -xzf deps/sqlite.tar.gz -C deps/sqlite --strip-components=1

CFLAGS=(-O3 -fno-plt -g0)
SQLITE_OPTS=(
  -DSQLITE_THREADSAFE=0            # single thread owns SQLite; access is serialized
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_USE_ALLOCA
)
SERVER_OPTS=(-std=gnu11 -Wall -Wextra -Werror -Isrc -Ideps/sqlite)

compile() { # compile <output> <extra flags...>
  local out=$1; shift
  gcc "${CFLAGS[@]}" "${SQLITE_OPTS[@]}" -w "$@" -c deps/sqlite/sqlite3.c -o build/sqlite3.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/server.c -o build/server.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/api.c -o build/api.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/db.c -o build/db.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/jwt.c -o build/jwt.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/json.c -o build/json.o
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/http.c -o build/http.o
  gcc "${CFLAGS[@]}" "$@" build/server.o build/api.o build/db.o build/jwt.o \
      build/json.o build/http.o build/sqlite3.o -o "$out"
}

rm -rf build && mkdir -p build
compile build/server-train -fprofile-generate
./build/server-train --pgo-train build/train.db
compile bin/server -flto -fprofile-use -fprofile-partial-training -Wno-missing-profile
rm -f build/train.db*
echo "built bin/server"
