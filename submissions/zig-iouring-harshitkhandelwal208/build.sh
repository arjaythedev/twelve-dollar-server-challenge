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

PGO_FLAG=""
PROFDATA_CMD=""
for cmd in llvm-profdata llvm-profdata-20 llvm-profdata-19 llvm-profdata-18 llvm-profdata-17; do
  if command -v "$cmd" >/dev/null 2>&1; then
    PROFDATA_CMD="$cmd"
    break
  fi
done

# If portable proftext is available and llvm-profdata is present, regenerate runner-native profdata
if [ -f src/c/sqlite.proftext ] && [ -n "$PROFDATA_CMD" ]; then
  $PROFDATA_CMD merge -output=src/c/sqlite.profdata src/c/sqlite.proftext 2>/dev/null || true
fi

if [ -f src/c/sqlite.profdata ] && command -v clang >/dev/null 2>&1; then
  echo "== compiling PGO SQLite shim with clang =="
  if clang -c src/shim.c \
    -I deps/$SQLITE \
    -O3 -march=native -fno-stack-protector -fomit-frame-pointer -fno-semantic-interposition \
    -fprofile-use=src/c/sqlite.profdata \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
    -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
    -o src/c/shim_pgo.o 2>/dev/null; then
    PGO_FLAG="-Dpgo=true"
  else
    echo "Notice: Clang PGO build skipped; falling back to native Zig build"
    rm -f src/c/shim_pgo.o
  fi
fi

zig build -Doptimize=ReleaseFast -Dcpu=native $PGO_FLAG
