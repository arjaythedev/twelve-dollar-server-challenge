#!/usr/bin/env bash
# Builds the app as a normal user:
#  1. SQLite 3.53.4 from the official amalgamation (pinned by sha256) into build/native/libsqlite3.so,
#     with SQLite's recommended compile-time options for speed;
#  2. the Kotlin server with Gradle, which fetches the pinned dependencies from Maven Central and
#     installs the jars under build/install/server/lib.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=3530400
SQLITE_SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
mkdir -p build/native
if [ ! -f "build/native/sqlite-$SQLITE.zip" ]; then
  curl -fsSL -o "build/native/sqlite-$SQLITE.zip.tmp" "https://sqlite.org/2026/sqlite-amalgamation-$SQLITE.zip"
  mv "build/native/sqlite-$SQLITE.zip.tmp" "build/native/sqlite-$SQLITE.zip"
fi
echo "$SQLITE_SHA256  build/native/sqlite-$SQLITE.zip" | sha256sum -c --quiet -
unzip -oq -d build/native "build/native/sqlite-$SQLITE.zip"
# THREADSAFE=2: no per-connection mutex (each connection is used by one thread); MEMSTATUS=0: no global
# mutex and accounting on every malloc; HAVE_USLEEP: the busy handler sleeps in ms, not whole seconds.
gcc -O2 -fPIC -shared -o build/native/libsqlite3.so "build/native/sqlite-amalgamation-$SQLITE/sqlite3.c" \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DHAVE_USLEEP=1 -DSQLITE_DQS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA -lpthread -lm

/opt/gradle-9.8.1/bin/gradle --no-daemon --console=plain -q installDist
