#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps src/c

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL --retry 3 -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

PGO_FLAG=""
PROFDATA_CMD=""
for cmd in llvm-profdata llvm-profdata-20 llvm-profdata-19 llvm-profdata-18 llvm-profdata-17; do
  if command -v "$cmd" >/dev/null 2>&1; then
    PROFDATA_CMD="$cmd"
    break
  fi
done

# Dynamic on-box PGO profile generation (Rule 10 compliant)
if command -v clang >/dev/null 2>&1 && [ -n "$PROFDATA_CMD" ] && [ -f src/c/pgo_train.c ]; then
  echo "== Recording SQLite PGO profile with Clang on runner (Rule 10 compliant) =="
  TMP_PGO="$(mktemp -d)"
  if clang -O3 -march=native \
    -I deps/$SQLITE \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
    -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
    -fprofile-generate="$TMP_PGO/prof" \
    src/shim.c src/c/pgo_train.c -lpthread -ldl -lm -o "$TMP_PGO/pgo_train" 2>/dev/null; then

    # pgo_train creates and populates a 1,000-post multi-level B-tree scratch database internally
    "$TMP_PGO/pgo_train" "$TMP_PGO/scratch.db" 2>/dev/null || true
    $PROFDATA_CMD merge -output="$TMP_PGO/sqlite.profdata" "$TMP_PGO/prof"/*.profraw 2>/dev/null || true

    if [ -f "$TMP_PGO/sqlite.profdata" ]; then
      if clang -c src/shim.c \
        -I deps/$SQLITE \
        -O3 -march=native -fno-stack-protector -fomit-frame-pointer -fno-semantic-interposition \
        -fprofile-use="$TMP_PGO/sqlite.profdata" \
        -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
        -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
        -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
        -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
        -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
        -o src/c/shim_pgo.o 2>/dev/null; then
        PGO_FLAG="-Dpgo=true"
        echo "== Compiled PGO SQLite shim successfully with Clang =="
      fi
    fi
  fi
  rm -rf "$TMP_PGO"
fi

# Robust GCC PGO fallback if Clang PGO failed or runtime library was absent
if [ -z "$PGO_FLAG" ] && command -v gcc >/dev/null 2>&1 && [ -f src/c/pgo_train.c ]; then
  echo "== Recording SQLite PGO profile with GCC on runner (Rule 10 compliant) =="
  TMP_PGO="$(mktemp -d)"
  if gcc -O3 -march=native \
    -I deps/$SQLITE \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
    -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
    -fprofile-generate="$TMP_PGO/prof" \
    src/shim.c src/c/pgo_train.c -lpthread -ldl -lm -o "$TMP_PGO/pgo_train" 2>/dev/null; then

    "$TMP_PGO/pgo_train" "$TMP_PGO/scratch.db" 2>/dev/null || true
    if gcc -c src/shim.c \
      -I deps/$SQLITE \
      -O3 -march=native -fno-stack-protector -fomit-frame-pointer -fno-semantic-interposition \
      -fprofile-use="$TMP_PGO/prof" -fprofile-correction \
      -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
      -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
      -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
      -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
      -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
      -o src/c/shim_pgo.o 2>/dev/null; then
      PGO_FLAG="-Dpgo=true"
      echo "== Compiled PGO SQLite shim successfully with GCC =="
    fi
  fi
  rm -rf "$TMP_PGO"
fi

if [ -z "$PGO_FLAG" ]; then
  echo "Notice: Clang PGO build skipped; falling back to native Zig build"
  rm -f src/c/shim_pgo.o
fi

zig build -Doptimize=ReleaseFast -Dcpu=native $PGO_FLAG
