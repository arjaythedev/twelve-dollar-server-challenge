#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned SQLite amalgamation
# (SHA-256 checked), compiles it, assembles the server and links them.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
SQLITE_URL="https://www.sqlite.org/2026/$SQLITE.zip"

mkdir -p bin vendor
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "$SQLITE_URL"
  echo "$SQLITE_SHA  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -qo "vendor/$SQLITE.zip" -d vendor
  rm -f "vendor/$SQLITE.zip"
fi

# THREADSAFE=0: every worker process is single-threaded and owns its own
# connection (it forks before opening the database), so SQLite's per-call
# mutexes only cost time. File locking still serialises writers across
# processes, which is what WAL needs.
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_CACHE_SIZE=-1024"

if [ ! -f bin/sqlite3.o ] || [ "$SQLITE_FLAGS" != "$(cat bin/.sqlite-flags 2>/dev/null)" ] ||
   [ "vendor/$SQLITE/sqlite3.c" -nt bin/sqlite3.o ]; then
  gcc -O2 -march=x86-64 -fno-plt -w $SQLITE_FLAGS -I"vendor/$SQLITE" \
    -c "vendor/$SQLITE/sqlite3.c" -o bin/sqlite3.o
  printf '%s' "$SQLITE_FLAGS" > bin/.sqlite-flags
fi

for f in server util json crypto jwt db; do
  nasm -f elf64 -Isrc -o "bin/$f.o" "src/$f.asm"
done

gcc -no-pie -o bin/server \
  bin/server.o bin/util.o bin/json.o bin/crypto.o bin/jwt.o bin/db.o bin/sqlite3.o \
  -lpthread -lm

echo "built bin/server"
