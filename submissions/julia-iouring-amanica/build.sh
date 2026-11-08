#!/usr/bin/env bash
# Builds as a normal user: the pinned SQLite amalgamation into build/libjr.so.
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
DL="${JR_DL_CACHE:-build/dl}"    # download cache (the dev container mounts a volume here)
mkdir -p "$DL" build

CFLAGS="-O2 -fPIC -DNDEBUG -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1"
stamp="$(echo "$SQLITE_ID $CFLAGS $READ_BACKEND" | sha256sum | cut -c1-16)"

if [ ! -f build/libjr.so ] || [ "$(cat build/libjr.stamp 2>/dev/null)" != "$stamp" ]; then
  zip="$DL/sqlite-amalgamation-$SQLITE_ID.zip"
  [ -f "$zip" ] || { curl -fsSL -o "$zip.part" "$SQLITE_URL"; mv "$zip.part" "$zip"; }
  got="$(openssl dgst -sha3-256 -r "$zip" | cut -d' ' -f1)"
  [ "$got" = "$SQLITE_SHA3_256" ] || { echo "sqlite checksum mismatch: $got" >&2; rm -f "$zip"; exit 1; }
  rm -rf build/sqlite && mkdir -p build/sqlite && unzip -q -j "$zip" -d build/sqlite
  echo "compiling SQLite $SQLITE_VERSION"
  # shellcheck disable=SC2086
  cc $CFLAGS -shared -o build/libjr.so.tmp build/sqlite/sqlite3.c -lpthread -lm
  mv build/libjr.so.tmp build/libjr.so
  echo "$stamp" > build/libjr.stamp
fi
"$JULIA_HOME/bin/julia" --version
