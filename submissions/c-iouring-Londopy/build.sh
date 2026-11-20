#!/usr/bin/env bash
# Builds bin/server. Fetches the pinned SQLite amalgamation on first run and
# checks its SHA-256 (set SQLITE_TARBALL to a local copy to build offline).
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VER=3540100
SQLITE_URL="https://sqlite.org/2026/sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_SHA256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e

mkdir -p deps bin
if [ ! -f deps/sqlite3.c ]; then
  tarball="${SQLITE_TARBALL:-deps/sqlite-autoconf-${SQLITE_VER}.tar.gz}"
  [ -f "$tarball" ] || curl -fsSL "$SQLITE_URL" -o "$tarball"
  echo "${SQLITE_SHA256}  ${tarball}" | sha256sum -c --quiet -
  tar -xzf "$tarball" -C deps --strip-components=1 \
    "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" "sqlite-autoconf-${SQLITE_VER}/sqlite3.h"
fi

CC="${CC:-gcc}"
CFLAGS="-O2 -g -march=x86-64-v3 -mtune=cascadelake -fno-plt -fno-semantic-interposition -DNDEBUG -Wall -Wextra ${EXTRA_CFLAGS:-}"
SQLITE_OPTS=(
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_TEMP_STORE=3 -DSQLITE_USE_ALLOCA
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_JSON
)

# db.c #includes sqlite3.c; SQLite's own sources aren't warning-clean under -Wextra.
$CC $CFLAGS -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-function \
  "${SQLITE_OPTS[@]}" -Ideps -c src/db.c -o bin/db.o
$CC $CFLAGS -Ideps -c src/server.c -o bin/server.o
$CC $CFLAGS bin/db.o bin/server.o -o bin/server -lpthread -lm
echo "built $(pwd)/bin/server"
