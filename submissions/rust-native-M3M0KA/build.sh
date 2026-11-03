#!/usr/bin/env bash
# Builds the server as a normal user. Fetches the pinned SQLite amalgamation once,
# checksum-verifies it, compiles it with the system C compiler, then links the
# Rust binary against that object. No crates, no network beyond the tar.
#
#   bash build.sh             # full build with PGO on SQLite
#   bash build.sh --no-pgo    # plain build
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build bin

SQLITE_ZIP=sqlite-amalgamation-3540100.zip
SQLITE_URL="https://www.sqlite.org/2026/$SQLITE_ZIP"
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

if [ ! -f build/sqlite3.c ]; then
  [ -f "build/$SQLITE_ZIP" ] || curl -fsSL --retry 3 -o "build/$SQLITE_ZIP" "$SQLITE_URL"
  echo "$SQLITE_SHA  build/$SQLITE_ZIP" | sha256sum -c -
  rm -rf build/sqlite-amalgamation-3540100
  (cd build && unzip -q "$SQLITE_ZIP")
  cp build/sqlite-amalgamation-3540100/sqlite3.c build/sqlite3.c
  cp build/sqlite-amalgamation-3540100/sqlite3.h build/sqlite3.h
fi

CC=${CC:-cc}
ARCH=${FEED_ARCH:--march=native}
BASE="-O3 -g0 -DNDEBUG $ARCH -fno-plt -fno-semantic-interposition -fomit-frame-pointer"

# One connection per thread, no statistics, WAL by default, and the features
# this API never touches compiled out.
SQLITE_DEFS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_TRACE \
 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_COMPLETE \
 -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_OMIT_JSON \
 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
 -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"

sqlite_obj() { # $1 extra flags, $2 output
  # The shim includes sqlite3.c, so this one object carries SQLite plus the
  # accessors the Rust B-tree read paths call.
  $CC $BASE $SQLITE_DEFS $1 -Ibuild -c src/sqlite_shim.c -o "$2"
}

RUSTFLAGS_BASE="-C opt-level=3 -C codegen-units=1 -C lto=fat -C panic=abort \
 -C target-cpu=native -C force-frame-pointers=no"

rust_build() { # $1 extra rustc flags
  rustc --edition 2021 $RUSTFLAGS_BASE $1 src/main.rs -o bin/feed \
    -C link-arg=build/sqlite3.o -C link-arg=-lm -C link-arg=-lpthread
}

if [ "${1:-}" = "--no-pgo" ]; then
  sqlite_obj "" build/sqlite3.o
  rust_build ""
else
  if (
    set -e
    rm -rf build/pgo
    mkdir -p build/pgo
    sqlite_obj "-fprofile-generate -fprofile-update=single -fprofile-dir=build/pgo" build/sqlite3.o
    $CC -O2 -Ibuild -fprofile-generate -fprofile-update=single -fprofile-dir=build/pgo \
      src/pgo_train.c build/sqlite3.o -o build/pgo_train -lm -lpthread
    ./build/pgo_train build/train.db
    sqlite_obj "-fprofile-use -fprofile-partial-training -Wno-missing-profile -fprofile-dir=build/pgo" build/sqlite3.o
  ); then
    rust_build ""
  else
    echo "PGO step failed; building without a profile" >&2
    sqlite_obj "" build/sqlite3.o
    rust_build ""
  fi
fi

echo "built bin/feed"
