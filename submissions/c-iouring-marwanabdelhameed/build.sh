#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads two pinned sources (checked against SHA-256), builds liburing
# as a static library, compiles the SQLite amalgamation into the binary, then compiles src/*.c.
set -euo pipefail
cd "$(dirname "$0")"
DEPS="$PWD/deps"
mkdir -p "$DEPS" bin

SQLITE_VER=3500400
SQLITE_TGZ="sqlite-autoconf-$SQLITE_VER.tar.gz"
SQLITE_URL="https://www.sqlite.org/2025/$SQLITE_TGZ"
SQLITE_SHA256=a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18

URING_VER=2.5
URING_TGZ="liburing-$URING_VER.tar.gz"
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz"
URING_SHA256=456f5f882165630f0dc7b75e8fd53bd01a955d5d4720729b4323097e6e9f2a98

fetch() { # fetch <url> <file> <sha256>
  if [ ! -f "$DEPS/$2" ]; then
    curl -fsSL --retry 3 -o "$DEPS/$2.part" "$1"
    mv "$DEPS/$2.part" "$DEPS/$2"
  fi
  echo "$3  $DEPS/$2" | sha256sum -c --quiet -
}
fetch "$SQLITE_URL" "$SQLITE_TGZ" "$SQLITE_SHA256"
fetch "$URING_URL" "$URING_TGZ" "$URING_SHA256"

if [ ! -f "$DEPS/liburing/src/liburing.a" ]; then
  rm -rf "$DEPS/liburing"; mkdir -p "$DEPS/liburing"
  tar -xzf "$DEPS/$URING_TGZ" -C "$DEPS/liburing" --strip-components=1
  (cd "$DEPS/liburing" && ./configure >/dev/null && make -C src -j"$(nproc)" liburing.a >/dev/null)
fi

if [ ! -f "$DEPS/sqlite3.o" ]; then
  rm -rf "$DEPS/sqlite"; mkdir -p "$DEPS/sqlite"
  tar -xzf "$DEPS/$SQLITE_TGZ" -C "$DEPS/sqlite" --strip-components=1
  gcc -O2 -c -o "$DEPS/sqlite3.o" "$DEPS/sqlite/sqlite3.c" \
    -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_USE_ALLOCA
fi

gcc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -o bin/server \
  -I"$DEPS/sqlite" -I"$DEPS/liburing/src/include" \
  src/main.c src/app.c src/json.c src/sha256.c "$DEPS/sqlite3.o" "$DEPS/liburing/src/liburing.a" -lm
echo "built bin/server"
