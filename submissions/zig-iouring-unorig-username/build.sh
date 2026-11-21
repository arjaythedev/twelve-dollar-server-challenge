#!/usr/bin/env bash
# Builds bin/server: the server in Zig (ReleaseFast), plus src/btree.c, which includes SQLite 3.54.1 (the official
# amalgamation, SHA-256 pinned) and is compiled by gcc.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p bin deps
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi
MARCH=; [ "$(uname -m)" != x86_64 ] || MARCH=-march=native  # built on the box it runs on
SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"
GCC="gcc -O3 $MARCH -fno-plt -w $SQLITE_FLAGS -Ideps/$SQLITE -c src/btree.c -o deps/btree.o"
ZIG="zig build-exe -O ReleaseFast -lc -Ideps/$SQLITE --cache-dir .zig-cache"
# Profile-guided SQLite: an instrumented build of src/btree.c is linked into src/train.zig, which serves a request mix
# through the server's own code against a synthetic database it creates (no challenge data); then src/btree.c is
# compiled again with that profile and linked into the server. Zig code itself gets no profile (Zig has no PGO).
rm -f deps/*.gcda
$GCC -fprofile-generate -fprofile-update=single
$ZIG --name train -femit-bin=deps/train src/train.zig deps/btree.o "$(gcc -print-file-name=libgcov.a)" "$(gcc -print-libgcc-file-name)"
PGO_TRAIN_DB=deps/train.db deps/train
rm -f deps/train deps/train.db*
$GCC -fprofile-use -fprofile-partial-training -Wno-missing-profile
$ZIG -fstrip --name server -femit-bin=bin/server src/main.zig deps/btree.o
