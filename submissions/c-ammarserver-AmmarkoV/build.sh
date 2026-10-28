#!/usr/bin/env bash
# Runs as a normal user. Fetches the two pinned dependencies and builds ./build/feedapi .
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"
BUILD="$HERE/build"
mkdir -p "$BUILD"

# AmmarServer, the HTTP server this submission is built on. Pinned by commit.
# This commit has to be one that carries API mode *and* its io_uring transport ( src/AmmServerlib/threads/
# uringAPIServer.c , apiProtocol.c ) - without them the link below fails with an undefined reference to
# AmmServer_StartAPIOnRings, which is what feedapi.c starts the server with.
AMMARSERVER_REPO="https://github.com/AmmarkoV/AmmarServer.git"
AMMARSERVER_COMMIT="41555ecaddabfabd058e4a1be26655ca2d899152"

# SQLite, built from the amalgamation rather than taken from apt: it pins the version the benchmark runs
# against, and it is the only way to get the compile-time options below ( most of all THREADSAFE=0 ).
SQLITE_VERSION="3530000"
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip"
SQLITE_SHA256="bf3733d7c71b3ab0f6fd8a9ea0052ad87fa037d94333e14ce09878ba3492c3b0"

# ---------------------------------------------------------------- AmmarServer
AMMARSERVER="$BUILD/AmmarServer"

if [ -n "${AMMARSERVER_SRC:-}" ]; then
  # Build against a checkout that is already on this machine, for developing the two together. The
  # benchmark box and CI never set this and always build the pinned commit above.
  echo "== AmmarServer from \$AMMARSERVER_SRC=$AMMARSERVER_SRC"
  rm -rf "$AMMARSERVER"
  mkdir -p "$AMMARSERVER"
  cp -a "$AMMARSERVER_SRC/CMakeLists.txt" "$AMMARSERVER_SRC/src" "$AMMARSERVER/"
  # A development checkout usually has object files and libraries lying around in its source tree from
  # previous builds ; none of them are going anywhere near this binary.
  find "$AMMARSERVER/src" -type f \( -name '*.a' -o -name '*.so' -o -name '*.o' \) -delete
else
  echo "== AmmarServer @ $AMMARSERVER_COMMIT"
  if [ ! -d "$AMMARSERVER/.git" ]; then
    rm -rf "$AMMARSERVER"
    git init -q "$AMMARSERVER"
    git -C "$AMMARSERVER" remote add origin "$AMMARSERVER_REPO"
  fi
  git -C "$AMMARSERVER" fetch -q --depth 1 origin "$AMMARSERVER_COMMIT"
  git -C "$AMMARSERVER" -c advice.detachedHead=false checkout -q FETCH_HEAD
fi

# Said plainly here rather than left to surface as an undefined reference at the link step below.
for needed in epollAPIServer.c apiProtocol.c uringAPIServer.c; do
  if [ ! -f "$AMMARSERVER/src/AmmServerlib/threads/$needed" ]; then
    echo "build.sh: this AmmarServer checkout has no src/AmmServerlib/threads/$needed , which this" >&2
    echo "          submission is built on ( API mode and its io_uring transport )." >&2
    echo "          AMMARSERVER_COMMIT=$AMMARSERVER_COMMIT needs to point at a commit that carries it." >&2
    exit 1
  fi
done

# Only the AmmarServer library target, not the Services/ demo applications that ship alongside it.
cmake -S "$AMMARSERVER" -B "$AMMARSERVER/build" -DCMAKE_BUILD_TYPE=Release > "$BUILD/cmake.log"
make -C "$AMMARSERVER/build" -j"$(nproc)" AmmarServer > "$BUILD/make.log"

# The three static archives that build just produced. Named by full path rather than with -L/-l so that
# there is no question of which file ends up in the binary : -lAmmarServer would rather have a
# libAmmarServer.so sitting in the same directory, and nothing prebuilt gets linked here ( rule 10 ).
AMMARSERVER_LIBS=(
  "$AMMARSERVER/src/AmmServerlib/libAmmarServer.a"
  "$AMMARSERVER/src/Hashmap/libHashmap.a"
  "$AMMARSERVER/src/InputParser/libInputParser_C.a"
)
for lib in "${AMMARSERVER_LIBS[@]}"; do
  [ -f "$lib" ] || { echo "build.sh: $lib was not produced by the AmmarServer build" >&2; exit 1; }
done

# ---------------------------------------------------------------- SQLite
SQLITE="$BUILD/sqlite-amalgamation-${SQLITE_VERSION}"

if [ ! -f "$SQLITE/sqlite3.c" ]; then
  echo "== SQLite $SQLITE_VERSION"
  curl -fsSL -o "$BUILD/sqlite.zip" "$SQLITE_URL"
  echo "$SQLITE_SHA256  $BUILD/sqlite.zip" | sha256sum -c -
  unzip -oq "$BUILD/sqlite.zip" -d "$BUILD"
fi

# THREADSAFE=0        : one connection used from one thread ( AmmarServer's event loop ), so every mutex
#                       SQLite would otherwise take on the way to the data is compiled out
# DEFAULT_MEMSTATUS=0 : drops the global allocation counters, an atomic per malloc/free
# OMIT_AUTOINIT       : no "is the library initialized yet" check per API call ( main() calls
#                       sqlite3_initialize itself )
# DQS=0               : no double-quoted string literals, so a typo'd identifier is an error not a string
# USE_ALLOCA          : small scratch allocations go on the stack
# DEFAULT_WAL_SYNCHRONOUS=1 : NORMAL in WAL mode by default, which is what challenge rule 6 asks for and
#                       what main() sets explicitly anyway
SQLITE_CFLAGS=(
  -O2 -DNDEBUG -march=native -mtune=native
  -DSQLITE_THREADSAFE=0
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_AUTOINIT
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_DQS=0
  -DSQLITE_USE_ALLOCA
  -DSQLITE_TRUSTED_SCHEMA=0
)

# Recompiled when the source changes *or the flags do* : an object built with different flags is as stale as
# one built from different source, and sqlite3.o is the one thing here expensive enough to want to cache.
if [ ! -f "$BUILD/sqlite3.o" ] || \
   [ "$SQLITE/sqlite3.c" -nt "$BUILD/sqlite3.o" ] || \
   [ "${SQLITE_CFLAGS[*]}" != "$(cat "$BUILD/sqlite3.flags" 2>/dev/null)" ]; then
  echo "== compiling sqlite3.c"
  cc -c "${SQLITE_CFLAGS[@]}" -o "$BUILD/sqlite3.o" "$SQLITE/sqlite3.c"
  printf '%s' "${SQLITE_CFLAGS[*]}" > "$BUILD/sqlite3.flags"
fi

# ---------------------------------------------------------------- the API itself
# -march=native : everything here is compiled from source on the machine that will run it, so the binary is
# built for that exact CPU. It does mean the binary is only valid on the host that built it - if the
# instruction set under it ever changed, this would be a SIGILL rather than a slow server.
echo "== feedapi"
cc -O2 -DNDEBUG -Wall -march=native -mtune=native \
   -I"$SQLITE" \
   -I"$AMMARSERVER/src/AmmServerlib" \
   -o "$BUILD/feedapi" \
   feedapi.c jwt.c "$BUILD/sqlite3.o" \
   "${AMMARSERVER_LIBS[@]}" \
   -lcrypto -lpthread -lrt -lm

echo "== built $BUILD/feedapi"
