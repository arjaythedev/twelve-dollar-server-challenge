#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned SQLite amalgamation (sha256-checked),
# compiles it into the C shim, assembles the .asm files and links them.
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
SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_COMPLETE
  -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"
if [ ! -f bin/shim.o ] || [ src/shim.c -nt bin/shim.o ]; then
  gcc -O3 -march=native -fno-plt -w $SQLITE_FLAGS -Ivendor -c src/shim.c -o bin/shim.o
fi
for f in loop app db jwt json http out sha256; do
  nasm -f elf64 -Isrc/ -o bin/$f.o src/$f.asm
done
gcc -no-pie -o bin/server bin/loop.o bin/app.o bin/db.o bin/jwt.o bin/json.o bin/http.o bin/out.o \
  bin/sha256.o bin/shim.o -lpthread -lm
