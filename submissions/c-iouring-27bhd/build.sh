#!/usr/bin/env bash
# Builds the C io_uring server using Profile-Guided Optimization (PGO)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

mkdir -p bin

if [ ! -f "sqlite3.c" ] || [ ! -f "sqlite3.h" ]; then
    echo "Fetching SQLite amalgamation..."
    TMP_ZIP="/tmp/sqlite_amalgamation_$$.zip"
    curl -fsSL "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip" -o "$TMP_ZIP"
    unzip -q -o -j "$TMP_ZIP" -d "$DIR"
    rm -f "$TMP_ZIP"
fi

SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_DIRECT_OVERFLOW_READ -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DNDEBUG"
COMMON_FLAGS="-O3 -flto -fno-plt -fno-reorder-blocks-and-partition -march=native -mtune=native $SQLITE_FLAGS"

echo "== [1/3] Building instrumented binary for PGO training..."
gcc $COMMON_FLAGS -fprofile-generate server.c sqlite3.c -o bin/server -luring -lm

echo "== [2/3] Running full-pipeline PGO training workload..."
./bin/server --pgo-train

echo "== [3/3] Compiling optimized production binary with profile feedback..."
gcc $COMMON_FLAGS -fprofile-use -fprofile-correction -Wno-missing-profile server.c sqlite3.c -o bin/server -luring -lm

rm -f bin/*.gcda bin/*.gcno *.gcda *.gcno
echo "== Build complete: bin/server"