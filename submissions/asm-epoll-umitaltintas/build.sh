#!/usr/bin/env bash
# Builds SQLite (pinned amalgamation, C) and the server (x86-64 NASM), links them into bin/server.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE_VER=3500400
SQLITE_YEAR=2025
SQLITE_SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
mkdir -p build bin
if [ ! -f build/sqlite3.c ]; then
  curl -fsSL -o build/sqlite.zip "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VER}.zip"
  echo "${SQLITE_SHA256}  build/sqlite.zip" | sha256sum -c -
  unzip -oq build/sqlite.zip -d build
  cp "build/sqlite-amalgamation-${SQLITE_VER}/sqlite3.c" build/sqlite3.c
fi
if [ ! -f build/sqlite3.o ]; then
  gcc -O3 -march=native -fno-plt -c build/sqlite3.c -o build/sqlite3.o \
    -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT \
    -DSQLITE_USE_ALLOCA -DSQLITE_DEFAULT_MMAP_SIZE=1073741824 -DSQLITE_MAX_MMAP_SIZE=1073741824
fi
nasm -f elf64 -O2 -I src/ -o build/server.o src/server.asm
gcc -no-pie -o bin/server build/server.o build/sqlite3.o -lpthread -lm
