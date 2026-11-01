#!/usr/bin/env bash
# Builds SQLite from the pinned amalgamation and the server, both targeting x86-64-v3.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build bin
shards install
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f build/libsqlite3.a ] || [ src/native.c -nt build/libsqlite3.a ]; then
  [ -f build/$SQ.zip ] || curl -fsSL -o build/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  build/$SQ.zip" | sha256sum -c -
  (cd build && rm -rf $SQ && unzip -q $SQ.zip)
  # Multi-thread mode without mutexes (each connection stays on one thread), no memory statistics,
  # WAL defaults, and the unused features compiled out.
  FLAGS="-O3 -march=x86-64-v3 -fno-plt -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0 -DSQLITE_DEFAULT_FOREIGN_KEYS=1
    -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE
    -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_OMIT_JSON -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"
  # Profile-guided: build an instrumented SQLite, run pgo/train.c (the server's statements
  # on a synthetic database it generates), then rebuild SQLite with that profile.
  # bash build.sh --no-pgo skips it.
  if [ "${1:-}" = --no-pgo ]; then
    cc $FLAGS -Ibuild/$SQ -c src/native.c -o build/sqlite3.o
  else
    rm -rf build/pgo
    cc $FLAGS -fprofile-generate -fprofile-update=single -fprofile-dir=build/pgo -Ibuild/$SQ -c src/native.c -o build/sqlite3.o
    cc -O2 -Ibuild/$SQ pgo/train.c build/sqlite3.o -fprofile-generate -o build/train -lm
    ./build/train build/train.db
    cc $FLAGS -fprofile-use -fprofile-partial-training -Wno-missing-profile -fprofile-dir=build/pgo -Ibuild/$SQ -c src/native.c -o build/sqlite3.o
  fi
  ar rcs build/libsqlite3.a build/sqlite3.o
fi
# -Dgc_none: no garbage collector at all. The server allocates its buffers once at start-up
# and the request path never allocates, so there is nothing to collect.
crystal build --release --no-debug -Dgc_none -Dwithout_mt --mcpu x86-64-v3 --link-flags "-L$PWD/build -lm" src/main.cr -o bin/server
