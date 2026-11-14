#!/usr/bin/env bash
# Builds bin/server: SQLite 3.54.1 (official amalgamation) and liburing 2.12, both SHA-256 pinned,
# compiled from source together with the app using Profile-Guided Optimization (PGO).
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
URING=liburing-liburing-2.12
URING_SHA=f1d10cb058c97c953b4c0c446b11e9177e8c8b32a5a88b309f23fdd389e26370

mkdir -p bin deps

# Check local cache first, otherwise download
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  if [ -f "../../vendor-cache/$SQLITE.zip" ]; then
    cp "../../vendor-cache/$SQLITE.zip" deps/$SQLITE.zip
  elif [ -f "../c-iouring-unorig-username/deps/$SQLITE.zip" ]; then
    cp "../c-iouring-unorig-username/deps/$SQLITE.zip" deps/$SQLITE.zip
  else
    curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  fi
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

if [ ! -f deps/$URING/src/liburing.a ]; then
  if [ -f "../c-iouring-unorig-username/deps/liburing-2.12.tar.gz" ]; then
    cp "../c-iouring-unorig-username/deps/liburing-2.12.tar.gz" deps/liburing-2.12.tar.gz
  else
    curl -fsSL -o deps/liburing-2.12.tar.gz https://github.com/axboe/liburing/archive/refs/tags/liburing-2.12.tar.gz
  fi
  echo "$URING_SHA  deps/liburing-2.12.tar.gz" | sha256sum -c -
  tar -xzf deps/liburing-2.12.tar.gz -C deps
  (cd deps/$URING && ./configure >/dev/null && make -s -C src liburing.a >/dev/null)
fi

MARCH=; [ "$(uname -m)" != x86_64 ] || MARCH=-march=native

SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"

CC="gcc -O3 $MARCH -flto -fno-plt -w $SQLITE_FLAGS -Ideps/$SQLITE -Ideps/$URING/src/include \
  src/server.c deps/$URING/src/liburing.a -lpthread -lm -o bin/server -fprofile-dir=$PWD/deps/pgo"

rm -rf deps/pgo
echo "== compiling PGO instrumented binary"
$CC -DPGO_TRAIN -fprofile-generate -fprofile-update=single
echo "== training PGO profile (300k synthetic requests)"
PGO_TRAIN_DB=deps/train.db bin/server
rm -f deps/train.db*
echo "== compiling optimized production binary"
$CC -fprofile-use -fprofile-partial-training
echo "built bin/server ($(stat -c %s bin/server 2>/dev/null || stat -f %z bin/server) bytes)"
