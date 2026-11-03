#!/usr/bin/env bash
# Builds as a normal user:
#  1. SQLite from its official source (pinned, SHA-256 checked) into native/libsqlite3.so.
#  2. The server with dotnet publish (ReadyToRun) into out/..
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=3530400   # 3.53.4
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c

if [ ! -f native/libsqlite3.so ]; then
  mkdir -p native
  curl -fsSL -o native/sqlite.tar.gz "https://www.sqlite.org/2026/sqlite-autoconf-$SQLITE.tar.gz"
  echo "$SQLITE_SHA256  native/sqlite.tar.gz" | sha256sum -c -
  tar -xzf native/sqlite.tar.gz -C native --strip-components=1 "sqlite-autoconf-$SQLITE/sqlite3.c" "sqlite-autoconf-$SQLITE/sqlite3.h"
  # SQLite's recommended compile-time options, but THREADSAFE=2: two connections, each used by one thread at a time.
  FLAGS="-O2 -fPIC -fno-semantic-interposition -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION"
  # Profile-guided build (idea: #60): compile with profiling, run the server's statements on a synthetic database
  # (sqlite-train.c), then compile again with the profile. Same object path both times, so gcc finds the profile.
  gcc $FLAGS -fprofile-generate -fprofile-update=single -c native/sqlite3.c -o native/sqlite3.o
  gcc -O2 -Inative sqlite-train.c native/sqlite3.o -fprofile-generate -o native/train -lm
  (cd native && ./train train.db && rm -f train.db train.db-wal train.db-shm)
  gcc $FLAGS -fprofile-use -fprofile-partial-training -Wno-missing-profile -c native/sqlite3.c -o native/sqlite3.o
  gcc -shared -o native/libsqlite3.so native/sqlite3.o
  rm -f native/sqlite.tar.gz native/sqlite3.c native/sqlite3.h native/sqlite3.o native/*.gcda native/train
fi

export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
dotnet publish Server.fsproj -c Release --use-current-runtime --self-contained false -o out
cp native/libsqlite3.so out/
# Stop the compiler servers, so they do not keep memory on a 2 GB box.
dotnet build-server shutdown > /dev/null 2>&1 || true
