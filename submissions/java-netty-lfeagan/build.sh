#!/usr/bin/env bash
# Builds the app as a normal user: SQLite 3.54.0 from the official amalgamation as a shared
# library (called through Java's FFM API), then the Java code with Maven.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=/opt/jdk-25
MVN=/opt/maven/bin/mvn

SQLITE=sqlite-autoconf-3540000
SQLITE_SHA256=134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed
mkdir -p build
if [ ! -f "build/$SQLITE.tar.gz" ]; then
  curl -fsSL -o "build/$SQLITE.tar.gz.part" "https://www.sqlite.org/2026/$SQLITE.tar.gz"
  mv "build/$SQLITE.tar.gz.part" "build/$SQLITE.tar.gz"
fi
echo "$SQLITE_SHA256  build/$SQLITE.tar.gz" | sha256sum -c -
tar -xzf "build/$SQLITE.tar.gz" -C build "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"

# The recommended compile-time options from https://sqlite.org/compile.html, plus
# THREADSAFE=2: every connection is only ever used by one thread at a time.
gcc -O2 -fPIC -shared -o build/libsqlite3.so "build/$SQLITE/sqlite3.c" \
  -DSQLITE_THREADSAFE=2 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DQS=0 \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
  -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_USE_ALLOCA \
  -lpthread -lm

"$MVN" -B -q -ntp package
