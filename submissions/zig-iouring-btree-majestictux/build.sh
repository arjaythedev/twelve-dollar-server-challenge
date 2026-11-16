#!/usr/bin/env bash
# Builds bin/server: the app with Zig 0.17 (ReleaseFast) and SQLite 3.54.1 (official amalgamation,
# SHA-256 pinned) with gcc. The server is the Zig 0.17 io_uring port of zig-iouring-unorig-username
# merged with a B-tree read layer:
#   * the SQLite translation unit is src/sqlite_bt.c, which #includes the amalgamation and adds
#     B-tree cursor read helpers (the top-10 entries' read path); train.c profiles those too
#   * SQLite is compiled -O3 -march=native (the build runs on the benchmark box itself, so native
#     is that box's CPU — the #1 entry does the same) instead of -O2 -march=x86-64-v2
#   * the Zig binary is also built -mcpu native instead of x86_64_v2
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
TARGET=; MARCH=  # the benchmark box is x86-64
[ "$(uname -m)" != x86_64 ] || { TARGET="-target x86_64-linux-gnu.2.39 -mcpu native"; MARCH=-march=native; }
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"
# SQLite is built by gcc with profile-guided optimization: an instrumented build runs train.c (the
# server's statements AND its B-tree reads, in the benchmark mix, on a synthetic database), then the
# translation unit is recompiled with that profile and linked into the Zig binary.
GCC="gcc -O3 $MARCH -w $SQLITE_FLAGS -Ideps/$SQLITE -Isrc"
$GCC -fprofile-generate -fprofile-update=single -c src/sqlite_bt.c -o deps/sqlite3.o
$GCC -fprofile-generate train.c deps/sqlite3.o -o deps/train -lm
deps/train deps/train.db
$GCC -fprofile-use -fprofile-partial-training -Wno-missing-profile -c src/sqlite_bt.c -o deps/sqlite3.o
rm -f deps/train deps/train.db* deps/*.gcda
zig build-exe -O ReleaseFast $TARGET -lc -Ideps/$SQLITE -Isrc \
  -fstrip --cache-dir .zig-cache --name server -femit-bin=bin/server src/main.zig deps/sqlite3.o
