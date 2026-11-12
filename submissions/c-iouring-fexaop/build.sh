#!/usr/bin/env bash
# Builds the server as a normal user. Fetches the pinned SQLite amalgamation (sha256-checked),
# then compiles it and the server with this machine's tuning.
set -euo pipefail
cd "$(dirname "$0")"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor bin
if [ ! -f vendor/sqlite3.c ]; then
  curl -fsSL --retry 3 -o vendor/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  vendor/$SQ.zip" | sha256sum -c -
  (cd vendor && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi
CFLAGS="-O3 -march=native -fno-plt -DNDEBUG -std=c11 ${EXTRA_CFLAGS:-}"
SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
 -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
 -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE
 -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"
gcc $CFLAGS $SQLITE_FLAGS -Ivendor -Isrc \
    src/server.c src/app.c src/db.c src/http.c src/json.c src/jwt.c src/sha256.c \
    -o bin/feed-server -lpthread -lm -ldl
