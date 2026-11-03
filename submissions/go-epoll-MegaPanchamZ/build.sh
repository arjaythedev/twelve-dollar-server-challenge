#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-autoconf-3530400
SQLITE_SUM=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
mkdir -p .deps bin

if [ ! -f .deps/sqlite3.c ]; then
  if curl -fsSL --connect-timeout 5 --retry 2 "https://www.sqlite.org/2026/$SQLITE.tar.gz" -o .deps/sqlite.tar.gz 2>/dev/null; then
    echo "$SQLITE_SUM  .deps/sqlite.tar.gz" | sha256sum -c - >/dev/null
    tar -xzf .deps/sqlite.tar.gz -C .deps --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
    rm -f .deps/sqlite.tar.gz .deps/libsqlite3.a
  elif [ ! -f .deps/libsqlite3.a ] && [ -f /usr/include/sqlite3.h ] && [ -f /usr/lib/x86_64-linux-gnu/libsqlite3.a ]; then
    cp /usr/include/sqlite3.h .deps/sqlite3.h
  fi
fi

if [ -f .deps/sqlite3.c ]; then
  if [ ! -f .deps/libsqlite3.a ] || [ build.sh -nt .deps/libsqlite3.a ] || [ shim.h -nt .deps/libsqlite3.a ] || [ pgo/engine.c -nt .deps/libsqlite3.a ] || [ pgo/train.c -nt .deps/libsqlite3.a ]; then
    flags="-O3 -march=native -fno-semantic-interposition -fno-plt -fomit-frame-pointer -I.deps -DHAVE_SQLITE_AMALGAMATION -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_OMIT_FOREIGN_KEY -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_DECLTYPE -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1"
    profile="$PWD/.deps/pgo"
    rm -rf "$profile"
    cc $flags -fprofile-generate="$profile" -c pgo/engine.c -o .deps/engine.o
    cc -O3 -march=native -I.deps -fprofile-generate="$profile" pgo/train.c .deps/engine.o -lm -o .deps/train
    scratch="$(mktemp -d)"
    .deps/train "$scratch/train.db" "$(cat ../../schema.sql)"
    rm -rf "$scratch"
    cc $flags -fprofile-use="$profile" -fprofile-correction -Wno-missing-profile -c pgo/engine.c -o .deps/engine.o
    rm -f .deps/libsqlite3.a
    ar rcs .deps/libsqlite3.a .deps/engine.o
  fi
else
  cp /usr/lib/x86_64-linux-gnu/libsqlite3.a .deps/libsqlite3.a
  cc -O3 -march=native -I.deps -c pgo/engine.c -o .deps/engine.o
  ar rcs .deps/libsqlite3.a .deps/engine.o
fi

export PATH="/usr/local/go/bin:$PATH" GOTOOLCHAIN=local CGO_ENABLED=1
go build -trimpath -ldflags="-s -w" -o bin/server .
