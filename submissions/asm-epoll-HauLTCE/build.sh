#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin

SQLITE_VER="3530400"
SQLITE_TAR="sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_URL="https://sqlite.org/2026/${SQLITE_TAR}"
SQLITE_SHA256="0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c"

if [ ! -f "bin/sqlite3.o" ]; then
  if [ ! -f "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" ] && command -v curl >/dev/null 2>&1; then
    if curl -fsSL --connect-timeout 5 --max-time 30 "$SQLITE_URL" -o "$SQLITE_TAR" 2>/dev/null; then
      if echo "${SQLITE_SHA256}  ${SQLITE_TAR}" | sha256sum -c - >/dev/null 2>&1; then
        tar -xzf "$SQLITE_TAR"
      fi
      rm -f "$SQLITE_TAR"
    fi
  fi
  if [ -f "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" ]; then
    gcc -O3 -march=native -fno-plt \
      -DSQLITE_THREADSAFE=0 \
      -DSQLITE_DEFAULT_MEMSTATUS=0 \
      -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
      -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
      -DSQLITE_MAX_EXPR_DEPTH=0 \
      -DSQLITE_OMIT_DECLTYPE \
      -DSQLITE_OMIT_DEPRECATED \
      -DSQLITE_OMIT_PROGRESS_CALLBACK \
      -DSQLITE_OMIT_SHARED_CACHE \
      -DSQLITE_DQS=0 \
      -c "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" -o bin/sqlite3.o
    rm -rf "sqlite-autoconf-${SQLITE_VER}"
  fi
fi

if [ -f "bin/sqlite3.o" ]; then
  gcc -no-pie -O3 -march=native -Wall server.s bin/sqlite3.o -o bin/server -lcrypto -ldl -lpthread
else
  [ -e /usr/lib/x86_64-linux-gnu/libsqlite3.so ] && L=-lsqlite3 || L=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0
  gcc -no-pie -O3 -march=native -Wall server.s -o bin/server "$L" -lcrypto
fi
chmod +x bin/server