#!/usr/bin/env bash
# Builds the HolyPHP compiler from the source under toolchain/, then compiles
# server.hphp into bin/server. Runs as a normal user; writes only inside this
# folder.
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
ROOT="$PWD/toolchain"
BUILD="$PWD/build"
CFLAGS=(-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" bin

# 1. Embed the runtime sources and bundled stdlib into the compiler binary.
#    The generated header is a build artifact, not checked in.
"$CC" -O2 -std=c11 "$ROOT/scripts/genembed.c" -o "$BUILD/genembed"
"$BUILD/genembed" "$ROOT/src/hphpc/runtime_embed.h" "$ROOT/src/hphpc/runtime"

# 2. Build the compiler.
for src in "$ROOT"/src/hphpc/*.c; do
  obj="$BUILD/obj/$(basename "${src%.c}").o"
  "$CC" "${CFLAGS[@]}" -I"$ROOT/src/hphpc" -c "$src" -o "$obj"
done
"$CC" -O2 "$BUILD"/obj/*.o -o "$BUILD/hphp" -lm -lpthread

# 3. Build the pinned SQLite amalgamation with THREADSAFE=0. The server is
#    single-threaded, so the serialized build only pays mutex overhead on
#    every step/column call. Falls back to the system library if the download
#    or the build fails, so an offline box still produces a working server.
SQLITE_VER=3460100
SQLITE_ZIP="sqlite-amalgamation-$SQLITE_VER.zip"
SQLITE_SHA="77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784"
SQLITE_URL="https://www.sqlite.org/2024/$SQLITE_ZIP"
SQLITE_DIR="$BUILD/sqlite"
mkdir -p "$SQLITE_DIR"
if command -v unzip >/dev/null 2>&1 \
   && curl -fsSL -o "$BUILD/$SQLITE_ZIP" "$SQLITE_URL" 2>/dev/null \
   && echo "$SQLITE_SHA  $BUILD/$SQLITE_ZIP" | sha256sum -c - >/dev/null 2>&1 \
   && unzip -q -o "$BUILD/$SQLITE_ZIP" -d "$BUILD/sqlite-src" >/dev/null 2>&1 \
   && cat "$BUILD/sqlite-src/sqlite-amalgamation-$SQLITE_VER/sqlite3.c" \
          "$ROOT/src/hp_btree.c" > "$SQLITE_DIR/sqlite3_all.c" \
   && "$CC" -O2 -fno-strict-aliasing -std=c11 -DSQLITE_THREADSAFE=0 \
            -c "$SQLITE_DIR/sqlite3_all.c" -o "$SQLITE_DIR/sqlite3.o"; then
  ar rcs "$SQLITE_DIR/libsqlite3.a" "$SQLITE_DIR/sqlite3.o"
  export CPATH="$BUILD/sqlite-src/sqlite-amalgamation-$SQLITE_VER${CPATH:+:$CPATH}"
  export HPHP_SQLITE_LIB="$SQLITE_DIR/libsqlite3.a"
  echo "sqlite: built $SQLITE_VER with THREADSAFE=0"
else
  echo "sqlite: using the system library"
fi

# 4. Compile the challenge server.
"$BUILD/hphp" build server.hphp -o bin/server

echo "built: $PWD/bin/server"
