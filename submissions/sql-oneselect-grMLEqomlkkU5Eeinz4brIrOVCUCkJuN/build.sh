#!/usr/bin/env bash
# Builds bin/server as a normal user: downloads the pinned SQLite amalgamation (SHA-256 checked) and
# compiles it with the server as one program.
#
#   ./build.sh            bin/server
#   ./build.sh harness    bin/harness too (tests without a network: see README)
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
LIBURING=liburing-2.9
LIBURING_SHA256=897b1153b55543e8b92a5a3eb9b906537a5fedcf8afaf241f8b8787940c79f8d
mkdir -p vendor bin
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi
if [ ! -f "vendor/liburing/src/liburing.a" ]; then
  curl -fsSL --retry 3 -o "vendor/$LIBURING.tar.gz" "https://github.com/axboe/liburing/archive/refs/tags/$LIBURING.tar.gz"
  echo "$LIBURING_SHA256  vendor/$LIBURING.tar.gz" | sha256sum -c -
  rm -rf vendor/liburing && mkdir -p vendor/liburing
  tar -xzf "vendor/$LIBURING.tar.gz" -C vendor/liburing --strip-components=1
  (cd vendor/liburing && ./configure > /dev/null && make -s -C src liburing.a > /dev/null)
fi
# SQLite: each connection is used by one thread (the loop, the checkpointer), so no mutexes.
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0
  -DSQLITE_USE_ALLOCA -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DNDEBUG"
CFLAGS="-std=gnu11 -O2 -march=x86-64-v2 -Wall -Wno-unused-function -I vendor/$SQLITE -I vendor/liburing/src/include -I src $SQLOPTS"
gcc $CFLAGS -c "vendor/$SQLITE/sqlite3.c" -o bin/sqlite3.o
gcc $CFLAGS src/server.c bin/sqlite3.o vendor/liburing/src/liburing.a -o bin/server.new -lpthread -lm
mv bin/server.new bin/server
[ "${1:-}" = harness ] && gcc $CFLAGS src/harness.c bin/sqlite3.o -o bin/harness -lpthread -lm
echo "built bin/server"
