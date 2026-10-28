#!/usr/bin/env bash
set -euo pipefail
# SQLite is a separately counted dependency, identical across these submissions.
mkdir -p .deps bin
archive=.deps/sqlite.zip
[[ -f $archive ]] || curl -fsSL --retry 3 https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip -o "$archive"
printf '%s  %s\n' 1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d "$archive" | sha256sum -c -
[[ -f .deps/sqlite-amalgamation-3530400/sqlite3.c ]] || unzip -q "$archive" -d .deps
sqlite="$PWD/.deps/sqlite-amalgamation-3530400"
if [[ ! -f .deps/libsqlite3.a || sqlite-build.sh -nt .deps/libsqlite3.a ]]; then
  cc -O3 -DNDEBUG -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED \
    -c "$sqlite/sqlite3.c" -o .deps/sqlite3.o
  ar rcs .deps/libsqlite3.a .deps/sqlite3.o
fi
export CGO_CFLAGS="-O3 -I$sqlite"
export CGO_LDFLAGS="-L$PWD/.deps -lsqlite3 -lm -ldl -lpthread"
export SQLITE3_LIB_DIR="$PWD/.deps" SQLITE3_INCLUDE_DIR="$sqlite" SQLITE3_STATIC=1
