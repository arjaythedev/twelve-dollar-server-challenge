#!/usr/bin/env bash
# Builds bin/server: SQLite 3.54.1 (official amalgamation, SHA-256 pinned) compiled in with the app.
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
MARCH=; [ "$(uname -m)" != x86_64 ] || MARCH=-march=x86-64-v2  # the benchmark box is x86-64
# Profile-guided build: an instrumented binary serves a request mix against a synthetic database it
# creates (train.c, no challenge data), then the real binary is compiled with that profile.
CC="gcc -O2 $MARCH -flto -fno-plt -w $SQLITE_FLAGS -Ideps/$SQLITE server.c deps/$SQLITE/sqlite3.c -o bin/server
  -fprofile-dir=$PWD/deps/pgo"
rm -rf deps/pgo
$CC -DPGO_TRAIN -fprofile-generate -fprofile-update=single
PGO_TRAIN_DB=deps/train.db bin/server
rm -f deps/train.db*
$CC -fprofile-use -fprofile-partial-training
