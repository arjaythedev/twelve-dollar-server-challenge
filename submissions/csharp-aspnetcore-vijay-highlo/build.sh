#!/usr/bin/env bash
# Builds as a normal user: SQLite from its official source (pinned, checksummed) with SQLite's recommended
# compile-time options, then the server with Native AOT, which links that SQLite in statically.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=3530400   # 3.53.4
SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
if [ ! -f native/libsqlite3.a ]; then
  mkdir -p native
  curl -fsSL -o native/sqlite.tar.gz "https://www.sqlite.org/2026/sqlite-autoconf-$SQLITE.tar.gz"
  echo "$SHA256  native/sqlite.tar.gz" | sha256sum -c -
  tar -xzf native/sqlite.tar.gz -C native --strip-components=1 "sqlite-autoconf-$SQLITE/sqlite3.c"
  # https://www.sqlite.org/compile.html#recommended_compile_time_options, plus no extension loading.
  # THREADSAFE=0 is safe because the server runs every statement under one lock.
  clang -O2 -fPIC -c native/sqlite3.c -o native/sqlite3.o \
    -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_AUTOINIT -DSQLITE_STRICT_SUBTYPE=1 \
    -DSQLITE_OMIT_LOAD_EXTENSION
  ar rcs native/libsqlite3.a native/sqlite3.o
fi
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
dotnet publish -c Release --use-current-runtime -o out
