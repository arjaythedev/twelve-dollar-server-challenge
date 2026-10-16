#!/usr/bin/env bash
# Builds the server as a normal user: the pinned SQLite 3.54.0 amalgamation, then the V program.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-autoconf-3540000
SQLITE_SHA256=134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed
mkdir -p build bin
if [ ! -f build/sqlite3.o ]; then
  curl -fsSL "https://www.sqlite.org/2026/$SQLITE.tar.gz" -o build/sqlite.tar.gz
  echo "$SQLITE_SHA256  build/sqlite.tar.gz" | sha256sum -c -
  tar -xzf build/sqlite.tar.gz -C build --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  # Each connection is used by one thread (multi-thread mode), no memory statistics.
  cc -O3 -march=native -c build/sqlite3.c -o build/sqlite3.o \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
fi
/opt/v/v -prod -cc cc -o bin/server .
