#!/usr/bin/env bash
# Builds the server as a normal user: the pinned SQLite amalgamation into build/sqlite3.o,
# then the V program into bin/server. On Linux the build is profile-guided: an instrumented
# server serves a synthetic database to itself (`bin/server --train`), then everything is
# rebuilt with that profile. `bash build.sh --no-pgo` skips that. VBIN overrides the compiler
# path for local development; VFLAGS (read by V itself) adds compiler flags.
set -euo pipefail
cd "$(dirname "$0")"

VBIN="${VBIN:-/opt/twelve-v/v}"
CC="${CC:-cc}"
# This code is written for V's current compiler; a silent retry with the 0.5.2
# compatibility compiler would only hide the real error.
export V_MACOS_V3_NO_FALLBACK=1
PGO="${PGO:-1}"
# URING=0 builds the plain epoll backend instead of io_uring (Linux only either way).
URING="${URING:-1}"
VDEFS=""
[ "${1:-}" = "--no-pgo" ] && PGO=0
[ "$(uname -s)" = "Linux" ] || PGO=0
if [ "$(uname -s)" = "Linux" ] && [ "$URING" = "1" ]; then VDEFS="-d uring"; fi

SQLITE=sqlite-autoconf-3530400
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
SQLITE_CFLAGS="-O3 -DSQLITE_THREADSAFE=2 -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
 -DSQLITE_USE_ALLOCA -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0"
if [ "$(uname -s)" = "Linux" ]; then
  SQLITE_CFLAGS="$SQLITE_CFLAGS -DHAVE_FDATASYNC=1 -march=native"
fi

sha256_check() { # sha256_check <expected> <file>
  if command -v sha256sum >/dev/null 2>&1; then
    echo "$1  $2" | sha256sum -c -
  else
    echo "$1  $2" | shasum -a 256 -c -
  fi
}

mkdir -p build bin
if [ ! -f build/sqlite3.c ]; then
  curl -fsSL "https://www.sqlite.org/2026/$SQLITE.tar.gz" -o build/sqlite.tar.gz
  sha256_check "$SQLITE_SHA256" build/sqlite.tar.gz
  tar -xzf build/sqlite.tar.gz -C build --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"
  rm -f build/sqlite.tar.gz
fi

build_sqlite() { # build_sqlite <extra cflags>
  # shellcheck disable=SC2086
  "$CC" $SQLITE_CFLAGS $1 -c build/sqlite3.c -o build/sqlite3.o
}
if [ "$PGO" = "1" ]; then
  # gcc keys profile data on the object's path, and V names its build directory with a
  # random id, so the two passes cannot both go through V. Instead V generates the C once
  # (-keepc) and prints its own C compiler command line (-showcc); that same command is
  # then run twice from a fixed directory, instrumented and then profile-guided.
  PROF="$PWD/build/pgo"
  rm -rf "$PROF" build/tmp bin/server bin/.server.v3cc.* build/cgen
  mkdir -p "$PROF" build/tmp build/cgen
  build_sqlite ""
  TMPDIR="$PWD/build/tmp" "$VBIN" -prod -nocache -keepc -showcc -cc "$CC" $VDEFS -o bin/server src \
    > build/showcc.log 2>&1 || { cat build/showcc.log; exit 1; }
  build_sqlite "-fprofile-generate=$PROF"
  ccline="$(grep -E '^\s*> ' build/showcc.log | tail -1 | sed -E 's/^\s*> //')"
  kept="$(ls -t build/tmp/v_*/server.*.tmp.c | head -1)"
  cp "$kept" build/cgen/src.c
  rm -f bin/server
  case "$ccline" in
    *"-o out src.c"*) ;;
    *) echo "unexpected V cc command line: $ccline"; exit 1 ;;
  esac
  cc_pass() { # cc_pass <extra cflags>
    (cd build/cgen && eval "${ccline/-o out src.c/-o ../../bin/server src.c} -march=native $1")
  }
  cc_pass "-fprofile-generate=$PROF"
  JWT_SECRET=train-secret bin/server --train "$PWD/build/train.db"
  rm -f build/train.db build/train.db-wal build/train.db-shm bin/server
  build_sqlite "-fprofile-use=$PROF -fprofile-correction -Wno-missing-profile"
  cc_pass "-fprofile-use=$PROF -fprofile-correction -Wno-missing-profile"
  echo "built bin/server (PGO) with $("$VBIN" version)"
else
  if [ ! -f build/sqlite3.o ] || [ build/sqlite3.c -nt build/sqlite3.o ]; then
    build_sqlite ""
  fi
  "$VBIN" -prod -cc "$CC" $VDEFS -o bin/server src
  echo "built bin/server with $("$VBIN" version)"
fi
