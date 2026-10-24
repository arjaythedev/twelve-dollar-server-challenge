#!/usr/bin/env bash
# Builds the pinned SQLite and liburing libraries, then the V server.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-autoconf-3540000
SQLITE_SHA256=134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed
mkdir -p build bin
if [ ! -f build/sqlite3.o ]; then
  curl -fsSL "https://www.sqlite.org/2026/$SQLITE.tar.gz" -o build/sqlite.tar.gz
  echo "$SQLITE_SHA256  build/sqlite.tar.gz" | sha256sum -c -
  tar -xzf build/sqlite.tar.gz -C build --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  # Each connection is used by one thread (multi-thread mode), no memory statistics, and the
  # remaining options from SQLite's recommended compile-time list that this server does not need.
  cc -O3 -march=native -c build/sqlite3.c -o build/sqlite3.o \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_USE_ALLOCA
fi
LIBURING_COMMIT=08468cc3830185c75f9e7edefd88aa01e5c2f8ab # liburing 2.9
LIBURING_SHA256=1679f29b086a7c1cf217aff92bc192dae9f3ed3de82a70848b93dc82494c75b8
if [ ! -f build/liburing/src/liburing.a ]; then
  mkdir -p build/liburing
  curl -fsSL "https://codeload.github.com/axboe/liburing/tar.gz/$LIBURING_COMMIT" -o build/liburing.tar.gz
  echo "$LIBURING_SHA256  build/liburing.tar.gz" | sha256sum -c -
  tar -xzf build/liburing.tar.gz -C build/liburing --strip-components=1
  (cd build/liburing && ./configure --use-libc && make -C src liburing.a)
fi
cc -O3 -g -fno-omit-frame-pointer -I build/liburing/src/include \
  -c uring_bridge.c -o build/uring_bridge.o
/opt/v/v -prod -cc cc -o bin/server .
