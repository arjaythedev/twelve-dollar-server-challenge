#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/bin"
mkdir -p "$BIN"

# Check if sqlite3 amalgamation exists, otherwise download pinned version
if [ ! -f "$HERE/src/sqlite3.c" ]; then
    echo "== Fetching SQLite 3.47.2 amalgamation"
    curl -fsSL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip -o "$HERE/sqlite.zip"
    unzip -q -o "$HERE/sqlite.zip" -d "$HERE"
    mv "$HERE/sqlite-amalgamation-3470200/sqlite3.c" "$HERE/src/sqlite3.c"
    mv "$HERE/sqlite-amalgamation-3470200/sqlite3.h" "$HERE/src/sqlite3.h"
    rm -rf "$HERE/sqlite.zip" "$HERE/sqlite-amalgamation-3470200"
fi

echo "== Compiling SQLite 3 with -DSQLITE_THREADSAFE=0"
gcc -O3 -flto -march=native -mtune=native \
    -DSQLITE_THREADSAFE=0 \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DQS=0 \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_USE_ALLOCA=1 \
    -c "$HERE/src/sqlite3.c" -o "$BIN/sqlite3.o"

echo "== Compiling C++23 io_uring server"
g++ -std=c++23 -O3 -flto -march=native -mtune=native \
    -fno-exceptions -fno-rtti \
    -fno-semantic-interposition \
    -I"$HERE/src" \
    "$HERE/src/db.cpp" \
    "$HERE/src/server.cpp" \
    "$HERE/src/main.cpp" \
    "$BIN/sqlite3.o" \
    -luring \
    -o "$BIN/server"

echo "== Build successful: $BIN/server"
