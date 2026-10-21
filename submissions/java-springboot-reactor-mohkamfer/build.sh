#!/usr/bin/env bash
# Builds, as a normal user:
#   build/native/libsqlite3.so  SQLite 3.53.4 compiled from the official amalgamation (checksum-pinned)
#   build/libs/app.jar          the Spring Boot app (Gradle downloads the pinned dependencies)
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_ZIP=sqlite-amalgamation-3530400.zip
SQLITE_URL="https://www.sqlite.org/2026/$SQLITE_ZIP"
SQLITE_SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

if [ ! -f build/native/libsqlite3.so ]; then
  mkdir -p build/native
  curl -fsSL -o "build/native/$SQLITE_ZIP" "$SQLITE_URL"
  echo "$SQLITE_SHA256  build/native/$SQLITE_ZIP" | sha256sum -c - >/dev/null
  unzip -q -o -j "build/native/$SQLITE_ZIP" '*/sqlite3.c' '*/sqlite3.h' -d build/native
  # sqlite.org's recommended options for speed; THREADSAFE=2 because no connection is ever shared
  # between threads at the same time
  gcc -O2 -fPIC -shared -o build/native/libsqlite3.so build/native/sqlite3.c \
    -DNDEBUG \
    -DSQLITE_THREADSAFE=2 \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_DQS=0 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 \
    -DSQLITE_OMIT_DECLTYPE \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_USE_ALLOCA \
    -DSQLITE_STRICT_SUBTYPE=1 \
    -lpthread -lm
  rm -f "build/native/$SQLITE_ZIP" build/native/sqlite3.c build/native/sqlite3.h
fi

export JAVA_HOME=/opt/jdk
/opt/gradle/bin/gradle --no-daemon --console=plain -q bootJar
