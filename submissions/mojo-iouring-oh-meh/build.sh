#!/usr/bin/env bash
# Builds the server as a normal user. Downloads the two pinned C dependencies
# (checksum-verified), compiles the shim with the SQLite amalgamation into a
# static library, then builds the Mojo binary against it.
set -euo pipefail
cd "$(dirname "$0")"
MOJO="${MOJO:-/opt/mojo/bin/mojo}"
command -v "$MOJO" >/dev/null || { echo "mojo not found at $MOJO (run install.sh)"; exit 1; }

TP=third_party
mkdir -p "$TP" bin

SQ_VER=3540100
SQ_ZIP="sqlite-amalgamation-$SQ_VER.zip"
SQ_URL="https://sqlite.org/2026/$SQ_ZIP"
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
LU_VER=2.15
LU_TGZ="liburing-$LU_VER.tar.gz"
LU_URL="https://github.com/axboe/liburing/archive/refs/tags/$LU_TGZ"
LU_SHA=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a

if [ ! -f "$TP/sqlite3.c" ]; then
  curl -fsSL -o "$TP/$SQ_ZIP" "$SQ_URL"
  echo "$SQ_SHA  $TP/$SQ_ZIP" | sha256sum -c -
  unzip -o -j -q "$TP/$SQ_ZIP" -d "$TP" "sqlite-amalgamation-$SQ_VER/sqlite3.c" "sqlite-amalgamation-$SQ_VER/sqlite3.h"
fi
if [ ! -f "$TP/liburing/src/liburing.a" ]; then
  curl -fsSL -o "$TP/$LU_TGZ" "$LU_URL"
  echo "$LU_SHA  $TP/$LU_TGZ" | sha256sum -c -
  rm -rf "$TP/liburing"
  tar -xzf "$TP/$LU_TGZ" -C "$TP"
  mv "$TP/liburing-liburing-$LU_VER" "$TP/liburing"
  (cd "$TP/liburing" && ./configure && make -C src -j"$(nproc)" liburing.a) > "$TP/liburing-build.log" 2>&1 \
    || { tail -30 "$TP/liburing-build.log"; echo "liburing build failed"; exit 1; }
fi

SQ_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1"
CFLAGS="${CFLAGS:--O3 -march=native -fno-plt}"

# $SQ_FLAGS and $CFLAGS are unquoted on purpose: they are lists of flags.
if [ ! -f "$TP/libshim.a" ] || [ src/shim.c -nt "$TP/libshim.a" ] || [ "$TP/sqlite3.c" -nt "$TP/libshim.a" ] || [ build.sh -nt "$TP/libshim.a" ]; then
  echo "== shim + sqlite"
  gcc $CFLAGS -fPIC -I"$TP" -I"$TP/liburing/src/include" $SQ_FLAGS -c src/shim.c -o "$TP/shim.o"
  ar rcs "$TP/libshim.a" "$TP/shim.o"
fi

echo "== mojo"
"$MOJO" build src/server.mojo -o bin/server -I src \
  -Xlinker "$PWD/$TP/libshim.a" -Xlinker "$PWD/$TP/liburing/src/liburing.a" -Xlinker -lm -Xlinker -lpthread
ls -la bin/server
