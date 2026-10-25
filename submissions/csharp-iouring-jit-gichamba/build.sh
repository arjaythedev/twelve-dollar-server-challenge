#!/usr/bin/env bash
# Builds the JIT server into bin/ as a normal user, with SQLite as a shared library next to it.
# SQLite is downloaded, checked against its SHA-256 and compiled with GCC profile-guided optimization.
set -euo pipefail
cd "$(dirname "$0")"

DOTNET=/usr/local/lib/dotnet-sdk-10.0.401/dotnet
if [ ! -x "$DOTNET" ]; then
  echo "The .NET SDK is missing: run install.sh as root first" >&2
  exit 1
fi
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
# NuGet and the CLI keep their files inside this folder, so the build works whatever HOME is.
export DOTNET_CLI_HOME="$PWD/obj/home" NUGET_PACKAGES="$PWD/obj/nuget"

SQLITE=sqlite-autoconf-3530400
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
mkdir -p obj/sqlite
if [ ! -f obj/sqlite/libsqlite3.so ]; then
  curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 -o obj/sqlite/sqlite.tar.gz \
    "https://www.sqlite.org/2026/$SQLITE.tar.gz"
  echo "$SQLITE_SHA256  obj/sqlite/sqlite.tar.gz" | sha256sum -c --quiet -
  tar -xzf obj/sqlite/sqlite.tar.gz -C obj/sqlite --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  # SQLite's recommended compile-time options, plus no mutexes: only one thread uses it.
  # OMIT_AUTOINIT means the app must call sqlite3_initialize itself. Db does, and so does the trainer.
  DEFS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
    -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK
    -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"
  CC="gcc -O3 -march=native -fPIC $DEFS"
  # Pass 1 builds an instrumented object and a trainer that runs the server's statements on a
  # throwaway synthetic database. Pass 2 rebuilds with the profile it wrote, and fails if the
  # profile is missing, so a build is never silently unoptimized. GCC names the profile after
  # the object, so both passes write the same object path. The trainer is not linked into the server.
  rm -rf obj/sqlite/pgo obj/sqlite/train.db obj/sqlite/train.db-wal obj/sqlite/train.db-shm
  $CC -fprofile-generate=obj/sqlite/pgo -c obj/sqlite/sqlite3.c -o obj/sqlite/sqlite3.o
  $CC -fprofile-generate=obj/sqlite/pgo -Iobj/sqlite pgo/train.c obj/sqlite/sqlite3.o -o obj/sqlite/train -lm
  obj/sqlite/train obj/sqlite/train.db
  rm -f obj/sqlite/train obj/sqlite/train.db obj/sqlite/train.db-wal obj/sqlite/train.db-shm
  $CC -fprofile-use=obj/sqlite/pgo -Werror=missing-profile -c obj/sqlite/sqlite3.c -o obj/sqlite/sqlite3.o
  gcc -shared obj/sqlite/sqlite3.o -o obj/sqlite/libsqlite3.so -lm
fi

"$DOTNET" publish -c Release -p:RestoreLockedMode=true -o bin
cp obj/sqlite/libsqlite3.so bin/libsqlite3.so
