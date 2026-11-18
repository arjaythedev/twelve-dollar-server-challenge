#!/usr/bin/env bash
# build.sh: builds bin/server as a normal user.
# Compiles the server together with the SQLite amalgamation as one
# translation unit (like c-iouring-pavlyhalim). SQLite is downloaded from
# sqlite.org with a pinned SHA-256.
#
#   PGO=0 ./build.sh        plain -O3 build (default: PGO off for simplicity)
#   ./build.sh              same (PGO requires a runnable Linux target)
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor bin

# Download the pinned SQLite amalgamation if not present.
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  echo "build.sh: downloading SQLite $SQLITE"
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" \
    "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi

CC=${CC:-gcc}
command -v "${CC%% *}" > /dev/null 2>&1 || { echo "CC=$CC not found; using gcc" >&2; CC=gcc; }
export CC

# Portable x86-64-v3 target (Ubuntu 24.04 baseline, AVX2 but not AVX-512
# frequency throttling). -march=native is only for local profiling.
ARCH="-march=x86-64-v3 -mtune=generic"

# SQLite: strip unused features for a smaller, faster build.
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3
  -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DNDEBUG"

CFLAGS="-std=gnu11 -O3 -g $ARCH -fno-plt -fno-semantic-interposition
  -fomit-frame-pointer -Wall -Wno-unused-function -Wno-unused-variable
  -Wno-unused-but-set-variable -I vendor/$SQLITE -I src $SQLOPTS"

LDFLAGS="-static -lpthread -lm -lrt"

echo "build.sh: compiling with $CC"
$CC $CFLAGS src/server.c src/main.c -o bin/server.new $LDFLAGS
mv bin/server.new bin/server
echo "built bin/server ($(stat -c %s bin/server 2>/dev/null || stat -f %z bin/server) bytes)"
