#!/usr/bin/env bash
# Builds bin/server with Zig: the app (ReleaseFast) and SQLite 3.54.1 (official amalgamation, SHA-256 pinned).
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p bin deps
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"
zig build-exe -O ReleaseFast -target x86_64-linux-gnu -mcpu x86_64_v2 -lc -Ideps/$SQLITE \
  -fstrip --cache-dir .zig-cache --name server -femit-bin=bin/server \
  src/main.zig -cflags -O2 -w $SQLITE_FLAGS -- deps/$SQLITE/sqlite3.c
