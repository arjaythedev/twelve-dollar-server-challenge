#!/usr/bin/env bash
# Builds bin/server: vendored libco (coroutines) + SQLite amalgamation + the
# server. libco compiles as C++, the handler layers as C, one g++ link.
set -euo pipefail
cd "$(dirname "$0")"

SRC=src
BUILD=build
DEPS=deps

SQLITE_VER=3540000
SQLITE_YEAR=2026
SQLITE_URL="https://sqlite.org/$SQLITE_YEAR/sqlite-amalgamation-$SQLITE_VER.zip"
SQLITE_SHA=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d

# -march=native: the challenge box builds and runs on the same machine.
SQLITE_CFLAGS="-O3 -march=native -fomit-frame-pointer -DSQLITE_THREADSAFE=2 \
 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK \
 -DHAVE_FDATASYNC=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_CACHE_SIZE=-131072"
CFLAGS="-O2 -march=native -fomit-frame-pointer"

fetch() { # url out sha
    if [ -s "$2" ]; then return 0; fi
    curl -fsSL "$1" -o "$2"
    echo "$3  $2" | sha256sum -c - >/dev/null || { echo "sha mismatch: $2"; rm -f "$2"; exit 1; }
}

mkdir -p "$DEPS" "$BUILD" bin

fetch "$SQLITE_URL" "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" "$SQLITE_SHA"
if [ ! -d "$DEPS/sqlite-amalgamation-$SQLITE_VER" ]; then
    unzip -q -o "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" -d "$DEPS"
fi
SQLITE_SRC="$DEPS/sqlite-amalgamation-$SQLITE_VER/sqlite3.c"
SQLITE_INC="-I$DEPS/sqlite-amalgamation-$SQLITE_VER"

echo "== sqlite3"
[ "$BUILD/sqlite3.o" -nt "$SQLITE_SRC" ] || cc $SQLITE_CFLAGS -c "$SQLITE_SRC" -o "$BUILD/sqlite3.o"

echo "== libco (vendored)"
LIB="$SRC/libco"
LIB_OBJS="$BUILD/co_routine.o $BUILD/co_epoll.o $BUILD/co_hook_sys_call.o \
$BUILD/co_comm.o $BUILD/coctx.o $BUILD/coctx_swap.o"
# a header edit must rebuild the objects that include it, so rebuild the set
# when anything in the vendored tree is newer than the stamp
if [ -n "$(find "$LIB" -newer "$BUILD/libco.stamp" -print -quit 2>/dev/null)" ] ||
   [ ! -f "$BUILD/libco.stamp" ]; then
    for f in co_routine co_epoll co_hook_sys_call co_comm coctx; do
        g++ -O2 -std=gnu++11 -w -c "$LIB/$f.cpp" -I"$LIB" -o "$BUILD/$f.o"
    done
    g++ -O2 -c "$LIB/coctx_swap.S" -o "$BUILD/coctx_swap.o"
    touch "$BUILD/libco.stamp"
fi

echo "== server"
if [ -n "$(find "$SRC" -maxdepth 1 -name '*.c' -newer "$BUILD/server.stamp" -print -quit 2>/dev/null)" ] ||
   [ "$SRC/server.cpp" -nt "$BUILD/server.stamp" ] || [ ! -f "$BUILD/server.stamp" ]; then
    for f in http db json jwt util; do
        cc $CFLAGS -D_GNU_SOURCE $SQLITE_INC -I"$SRC" -c "$SRC/$f.c" -o "$BUILD/$f.o"
    done
    g++ -O2 -std=gnu++11 -w $SQLITE_INC -I"$SRC" -c "$SRC/server.cpp" -o "$BUILD/server.o"
    touch "$BUILD/server.stamp"
fi

g++ -o bin/server "$BUILD/server.o" "$BUILD/http.o" "$BUILD/db.o" "$BUILD/json.o" \
    "$BUILD/jwt.o" "$BUILD/util.o" $LIB_OBJS "$BUILD/sqlite3.o" -lpthread -ldl -lm
echo "== built bin/server"

# development self-test (not part of the server binary)
cc -O2 -D_GNU_SOURCE -I"$SRC" -o "$DEPS/selftest" "$SRC/selftest.c" "$SRC/jwt.c" "$SRC/json.c" "$SRC/util.c" -lm || true
