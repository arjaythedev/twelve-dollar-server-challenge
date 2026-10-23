#!/usr/bin/env bash
# Builds as a normal user: compiles the pinned, checksum-verified SQLite release into a static
# library, then publishes one Native AOT binary to bin/feed.
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_NOLOGO=1

SQLITE=sqlite-autoconf-3530400
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
mkdir -p obj/sqlite
if [ ! -f obj/sqlite/libsqlite3.a ]; then
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/${SQLITE}.tar.gz" -o obj/sqlite/sqlite.tar.gz
  echo "${SQLITE_SHA256}  obj/sqlite/sqlite.tar.gz" | sha256sum -c -
  tar -xzf obj/sqlite/sqlite.tar.gz -C obj/sqlite --strip-components=1 "${SQLITE}/sqlite3.c" "${SQLITE}/sqlite3.h"
  # SQLite's recommended compile-time options, plus no mutexes: only one thread uses it.
  # OMIT_AUTOINIT means the app must call sqlite3_initialize itself, and Db does.
  clang -O2 -march=native -fPIC -c obj/sqlite/sqlite3.c -o obj/sqlite/sqlite3.o \
    -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
  ar rcs obj/sqlite/libsqlite3.a obj/sqlite/sqlite3.o
fi

dotnet publish -c Release -p:RestoreLockedMode=true -o bin
