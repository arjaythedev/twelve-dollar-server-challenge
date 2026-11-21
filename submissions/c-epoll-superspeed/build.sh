#!/usr/bin/env bash
# Builds bin/server. Downloads the pinned SQLite amalgamation on first use.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p build bin deps
if [ ! -f "deps/$SQLITE/sqlite3.c" ]; then
  if [ ! -f "deps/$SQLITE.zip" ]; then
    curl -fsSL --retry 3 --connect-timeout 10 \
      -o "deps/$SQLITE.zip" "https://www.sqlite.org/2026/$SQLITE.zip"
  fi
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo "deps/$SQLITE.zip" -d deps
fi

SQLITE_OPTS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK \
 -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
 -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1"

# The benchmark box is x86-64; v2 is a safe baseline (SSE4.2/POPCNT).
MARCH=""
if [ "$(uname -m)" = "x86_64" ]; then MARCH="-march=x86-64-v2"; fi

CFLAGS="-O3 $MARCH -flto -fno-plt -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter -I deps/$SQLITE"
LDFLAGS="-flto $MARCH"

gcc $CFLAGS $SQLITE_OPTS -c server.c -o build/server.o
gcc $CFLAGS $SQLITE_OPTS -w -c "deps/$SQLITE/sqlite3.c" -o build/sqlite3.o
gcc $LDFLAGS build/server.o build/sqlite3.o -o bin/server -lm

echo "built bin/server"
