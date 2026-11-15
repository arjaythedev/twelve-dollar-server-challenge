#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps src/c

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL --retry 3 -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

if [ -f src/c/sqlite.profdata ] && command -v clang >/dev/null 2>&1; then
  echo "== compiling PGO SQLite shim with clang =="
  clang -c src/shim.c \
    -I deps/$SQLITE \
    -O3 -march=native -fno-stack-protector -fomit-frame-pointer -fno-semantic-interposition \
    -fprofile-use=src/c/sqlite.profdata \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
    -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
    -o src/c/shim_pgo.o
  zig build -Doptimize=ReleaseFast -Dcpu=native -Dpgo=true
else
  zig build -Doptimize=ReleaseFast -Dcpu=native
fi
