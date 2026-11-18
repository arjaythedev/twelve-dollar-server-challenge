#!/usr/bin/env bash
# Builds as a normal user: fetches the pinned SQLite amalgamation (sha256-checked) and compiles
# shim.c, which #includes it unmodified, into libtdshim.so. The server itself is Lua: no build.
set -euo pipefail
cd "$(dirname "$0")"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p vendor
if [ ! -f vendor/sqlite3.c ]; then
  curl -fsSL --retry 3 -o "vendor/$SQ.zip" "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  vendor/$SQ.zip" | sha256sum -c -
  (cd vendor && unzip -qo "$SQ.zip" && mv "$SQ/sqlite3.c" "$SQ/sqlite3.h" . && rm -rf "$SQ" "$SQ.zip")
fi

${CC:-cc} -O3 -march=native -fno-plt -fPIC -shared -o libtdshim.so shim.c -lm -lpthread \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE \
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE -DHAVE_FDATASYNC -DHAVE_USLEEP
