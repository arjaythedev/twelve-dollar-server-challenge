#!/usr/bin/env bash
# Builds the server as a normal user: the pinned SQLite source as a static library, then the Go binary.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-autoconf-3530400
SQLITE_SUM=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
mkdir -p .deps
if [ ! -f .deps/sqlite3.c ]; then
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/$SQLITE.tar.gz" -o .deps/sqlite.tar.gz
  echo "$SQLITE_SUM  .deps/sqlite.tar.gz" | sha256sum -c - >/dev/null
  tar -xzf .deps/sqlite.tar.gz -C .deps --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  rm .deps/sqlite.tar.gz
fi
if [ ! -f .deps/libsqlite3.a ] || [ build.sh -nt .deps/libsqlite3.a ] || [ pgo/train.c -nt .deps/libsqlite3.a ]; then
  # One goroutine owns the connection, so SQLite needs no mutexes.
  flags="-O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION"
  # Profile guided build: an instrumented SQLite runs pgo/train.c on synthetic rows, then SQLite is
  # compiled again using that profile. Both compiles write the same object so the profile matches.
  profile="$PWD/.deps/pgo"
  rm -rf "$profile"
  cc $flags -fprofile-generate="$profile" -c .deps/sqlite3.c -o .deps/sqlite3.o
  cc -O2 -I.deps -fprofile-generate="$profile" pgo/train.c .deps/sqlite3.o -lm -o .deps/train
  scratch="$(mktemp -d)"
  .deps/train "$scratch/train.db" "$(cat ../../schema.sql)"
  rm -rf "$scratch"
  cc $flags -fprofile-use="$profile" -fprofile-correction -Wno-missing-profile -c .deps/sqlite3.c -o .deps/sqlite3.o
  rm -f .deps/libsqlite3.a
  ar rcs .deps/libsqlite3.a .deps/sqlite3.o
fi

export PATH="/usr/local/go/bin:$PATH" GOTOOLCHAIN=local CGO_ENABLED=1
go build -trimpath -o bin/server .
