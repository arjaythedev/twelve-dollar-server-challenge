#!/usr/bin/env bash
# build.sh — builds the eBPF object and the SQLite shim. Self-contained: everything it needs is in this folder,
# except the pinned SQLite amalgamation, which it fetches and checksums.
#   1. http.bpf.o : the five BPF programs (clang -target bpf), from src/http.bpf.c
#   2. engine.o   : src/unit_sqlite.c (#includes engine.c, which #includes sqlite3.c and reads B-trees directly)
#   3. bin/shim   : src/shim.c linked with libbpf + engine.o
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-gcc}
SQLITE_VERSION=3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-$SQLITE_VERSION.zip"
mkdir -p deps build bin

sha256() { if sha256sum --version >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

# 1. SQLite amalgamation (pinned, checksummed)
if [ ! -f deps/sqlite/sqlite3.c ]; then
  [ -f deps/sqlite.zip ] && [ "$(sha256 deps/sqlite.zip | cut -d' ' -f1)" = "$SQLITE_SHA256" ] || \
    curl -fsSL --retry 3 -o deps/sqlite.zip "$SQLITE_URL"
  [ "$(sha256 deps/sqlite.zip | cut -d' ' -f1)" = "$SQLITE_SHA256" ] || { echo "sqlite sha mismatch"; exit 1; }
  rm -rf deps/sqlite "deps/sqlite-amalgamation-$SQLITE_VERSION"
  unzip -q deps/sqlite.zip -d deps
  mkdir -p deps/sqlite
  cp "deps/sqlite-amalgamation-$SQLITE_VERSION"/sqlite3.[ch] deps/sqlite/
fi

# 2. BPF object
ARCH=$(uname -m)
clang -O2 -g -Wall -Wno-unused-function -target bpf -D__BPF__ \
  -I/usr/include/"$ARCH"-linux-gnu -c src/http.bpf.c -o bin/http.bpf.o

# 3. engine (unity TU: unit_sqlite.c -> engine.c -> sqlite3.c)
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0"
SQLOPTS="$SQLOPTS -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_LOAD_EXTENSION"
SQLOPTS="$SQLOPTS -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK"
SQLOPTS="$SQLOPTS -DSQLITE_OMIT_DECLTYPE -DHAVE_FDATASYNC=1"
$CC -O3 -march=native $SQLOPTS -I deps/sqlite -I src \
  -c src/unit_sqlite.c -o build/engine.o

# 4. shim
$CC -O2 -march=native -I src -I deps/sqlite \
  -c src/shim.c -o build/shim.o
$CC -O2 -o bin/shim build/shim.o build/engine.o -lbpf -lelf -lz -lpthread -lm

echo "built bin/shim and bin/http.bpf.o"
