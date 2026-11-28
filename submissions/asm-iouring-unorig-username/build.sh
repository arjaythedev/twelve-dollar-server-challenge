#!/usr/bin/env bash
# Builds bin/server: SQLite 3.54.1 (official amalgamation, SHA-256 pinned) compiled into shim.c with gcc,
# server.asm assembled with nasm, linked together against glibc.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
COMBO=${COMBO:-1}  # 1: one pwrite per commit's WAL frames (shim.c) and a 2048 send pool (server.asm)
mkdir -p bin deps
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi
MARCH=; [ "$(uname -m)" != x86_64 ] || MARCH=-march=native  # built on the box it runs on
SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"
gcc -O3 $MARCH -fno-plt -w $SQLITE_FLAGS -DWALBUF=$COMBO -Ideps/$SQLITE -c shim.c -o bin/shim.o
nasm -f elf64 -DCOMBO=$COMBO -o bin/server.o server.asm
gcc -no-pie bin/server.o bin/shim.o -lpthread -lm -o bin/server
