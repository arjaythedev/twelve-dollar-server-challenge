#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads the pinned SQLite amalgamation and checks its SHA-256.
set -euo pipefail
cd "$(dirname "$0")"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p build bin
if [ ! -f "build/$SQ/sqlite3.c" ]; then
  [ -f "build/$SQ.zip" ] || curl -fsSL -o "build/$SQ.zip" "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  build/$SQ.zip" | sha256sum -c -
  (cd build && unzip -q -o "$SQ.zip")
fi
SQLITE_DEFS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
CFLAGS="-O2 -march=native -flto -fno-plt"
# SQLite stays unmodified: src/sqlite_bt.c includes the amalgamation and adds read helpers on its cursor API.
[ build/sqlite_bt.o -nt build.sh ] && [ build/sqlite_bt.o -nt src/sqlite_bt.c ] ||
  gcc $CFLAGS $SQLITE_DEFS -I"build/$SQ" -c src/sqlite_bt.c -o build/sqlite_bt.o
gcc $CFLAGS -Wall -Wextra -I"build/$SQ" -c src/server.c -o build/server.o
gcc $CFLAGS -o bin/server build/server.o build/sqlite_bt.o -luring -lm -pthread
