#!/usr/bin/env bash
# Build bin/server from server.c (single translation unit). Run as a normal user.
set -euo pipefail

cd "$(dirname "$0")"

SQLITE_URL="https://sqlite.org/2026/sqlite-amalgamation-3540100.zip"
SQLITE_SHA256="0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9"

if [ ! -f sqlite/sqlite3.c ]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  echo "Fetching SQLite amalgamation 3.54.1..."
  curl -fL -o "$tmp/sqlite.zip" "$SQLITE_URL"

  if command -v sha256sum >/dev/null 2>&1; then
    echo "$SQLITE_SHA256  $tmp/sqlite.zip" | sha256sum -c - >/dev/null
  else
    echo "$SQLITE_SHA256  $tmp/sqlite.zip" | shasum -a 256 -c - >/dev/null
  fi

  mkdir -p sqlite
  unzip -o -j -q "$tmp/sqlite.zip" "*sqlite3.c" "*sqlite3.h" -d sqlite
fi



SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_COMPLETE \
  -DSQLITE_OMIT_DECLTYPE"

mkdir -p bin
case "$(uname -s)" in
  Linux)
    # PGO stage. GCC keys .gcda profile files on the OBJECT path, so the
    # trainer and the final server compile below use the same object file
    # (build/pgo/server.o): the gcda the trainer run produces is then exactly
    # the one -fprofile-use looks up. Any failure — trainer compile, training
    # run, missing profile, final compile — falls back to the plain build.
    pgo=0
    mkdir -p build/pgo
    rm -f build/pgo/*.gcda build/pgo/*.profraw
    if gcc -O3 -march=native -fno-plt -fno-pie -pthread $SQLITE_FLAGS -I runtime \
         -fprofile-generate -fprofile-dir="$PWD/build/pgo" -c train.c \
         -o build/pgo/server.o \
    && gcc -no-pie -pthread -fprofile-generate -fprofile-dir="$PWD/build/pgo" build/pgo/server.o -lm -o bin/train \
    && ./bin/train "$PWD/build/train.db" 6; then
      prof_file="$(find build/pgo -name '*.gcda' -print -quit 2>/dev/null || true)"
      if [ -n "$prof_file" ] &&
         gcc -O3 -march=native -fno-plt -fno-pie -pthread $SQLITE_FLAGS -I runtime \
             -fprofile-use="$PWD/build/pgo" -fprofile-correction \
             -Wno-missing-profile -Wno-error=coverage-mismatch \
             -c server.c -o build/pgo/server.o \
      && gcc -no-pie -pthread build/pgo/server.o -lm -o bin/server; then
        pgo=1
        gcc -O3 -march=native -fno-plt -fno-pie -pthread $SQLITE_FLAGS -I runtime \
            -c train.c -o build/pgo/bench.o \
        && gcc -no-pie -pthread build/pgo/bench.o -lm -o bin/bench \
        || echo "pgo: bench build unavailable" >&2
      fi
    fi
    if [ "$pgo" -ne 1 ]; then
      echo "pgo: unavailable, using plain build" >&2
      gcc -O3 -march=native -fno-plt -fno-pie -no-pie -pthread $SQLITE_FLAGS -I runtime server.c -lm -o bin/server
      gcc -O3 -march=native -fno-plt -fno-pie -no-pie -pthread $SQLITE_FLAGS -I runtime train.c -lm -o bin/bench
    fi
    ;;
  Darwin)
    clang -O3 -pthread $SQLITE_FLAGS -I runtime server.c -lm -o bin/server
    clang -O3 -pthread $SQLITE_FLAGS -I runtime train.c -lm -o bin/bench
    ;;
  *)
    echo "Unsupported platform: $(uname -s)" >&2
    exit 1
    ;;
esac
echo "Built bin/server"
