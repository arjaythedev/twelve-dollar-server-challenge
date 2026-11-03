#!/usr/bin/env bash
# Builds the server as a normal user. Downloads two pinned, checksummed source tarballs
# (SQLite amalgamation and liburing) and compiles everything from source into bin/server.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VER=3530400
SQLITE_URL="https://www.sqlite.org/2026/sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
URING_VER=2.15
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-${URING_VER}.tar.gz"
URING_SHA256=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a

mkdir -p deps bin
fetch() { # fetch <url> <sha256> <file>
  if [ ! -f "deps/$3" ] || ! echo "$2  deps/$3" | sha256sum -c --quiet - 2>/dev/null; then
    curl -fsSL --retry 3 -o "deps/$3" "$1"
    echo "$2  deps/$3" | sha256sum -c --quiet -
  fi
}
fetch "$SQLITE_URL" "$SQLITE_SHA256" sqlite.tar.gz
fetch "$URING_URL" "$URING_SHA256" liburing.tar.gz

rm -rf deps/sqlite deps/liburing
mkdir -p deps/sqlite deps/liburing
tar -xzf deps/sqlite.tar.gz -C deps/sqlite --strip-components=1
tar -xzf deps/liburing.tar.gz -C deps/liburing --strip-components=1

# liburing: static library only
(cd deps/liburing && ./configure --cc=gcc >/dev/null && make -s -C src -j"$(nproc)" liburing.a >/dev/null)

CFLAGS=(-O3 -march=native -fno-plt -g0)
SQLITE_OPTS=(
  -DSQLITE_THREADSAFE=0              # one thread at a time uses SQLite (the server serializes access)
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
SERVER_OPTS=(-std=gnu11 -Wall -Wextra -Wno-unused-function -Ideps/sqlite -Ideps/liburing/src/include)

# compile <output> <extra flags...>: SQLite and the server as separate objects (stable profile names), then link
compile() {
  local out=$1; shift
  gcc "${CFLAGS[@]}" "${SQLITE_OPTS[@]}" -w "$@" -c deps/sqlite/sqlite3.c -o build/sqlite3.o  # -w: third-party code
  gcc "${CFLAGS[@]}" "${SERVER_OPTS[@]}" "$@" -c src/server.c -o build/server.o
  gcc "${CFLAGS[@]}" -Wno-stringop-overread "$@" build/server.o build/sqlite3.o deps/liburing/src/liburing.a -lpthread -lm -o "$out"
}

# Profile-guided optimization: build an instrumented binary, run it over a synthetic database
# (src/train.h; no network), then rebuild using the recorded profile. SQLite gets ~15% faster.
rm -rf build && mkdir -p build
compile build/server-train -fprofile-generate
./build/server-train --pgo-train build/train.db
compile bin/server -flto -fprofile-use -fprofile-partial-training -Wno-missing-profile
rm -f build/train.db*
echo "built bin/server"
