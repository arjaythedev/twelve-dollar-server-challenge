#!/usr/bin/env bash
# Downloads the pinned SQLite amalgamation into csrc/ (unity-included by fastdb.c) and builds.
# Set PGO=1 to compile with -fprofile-use=pgo when a profile directory already exists.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_ZIP=sqlite-amalgamation-3540000.zip
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
mkdir -p .sqlite-download csrc bin pgo
if [ ! -f ".sqlite-download/$SQLITE_ZIP" ]; then
  curl -fsSL -A "twelve-dollar-challenge" -o ".sqlite-download/$SQLITE_ZIP.part" "https://www.sqlite.org/2026/$SQLITE_ZIP"
  mv ".sqlite-download/$SQLITE_ZIP.part" ".sqlite-download/$SQLITE_ZIP"
fi
# sha256sum on Linux; shasum on macOS
if command -v sha256sum >/dev/null; then
  echo "$SQLITE_SHA256  .sqlite-download/$SQLITE_ZIP" | sha256sum -c -
else
  echo "$SQLITE_SHA256  .sqlite-download/$SQLITE_ZIP" | shasum -a 256 -c -
fi
unzip -o -j -q ".sqlite-download/$SQLITE_ZIP" '*/sqlite3.c' '*/sqlite3.h' -d csrc

export PATH="/usr/local/go/bin:${PATH:-}" GOTOOLCHAIN=local CGO_ENABLED=1

EXTRA_CFLAGS=""
if [ "${PGO:-0}" = "1" ] && [ -d pgo ] && ls pgo/*.gcda >/dev/null 2>&1; then
  EXTRA_CFLAGS="-fprofile-use=pgo -fprofile-correction"
  echo "== using SQLite/GCC PGO profile in pgo/"
fi
export CGO_CFLAGS="-O3 -I${PWD} -I${PWD}/csrc ${EXTRA_CFLAGS} \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA \
  -DSQLITE_MAX_MMAP_SIZE=0"

GO_ARGS=(-trimpath -ldflags="-s -w" -o bin/server .)
if [ -f default.pgo ]; then
  GO_ARGS=(-pgo=default.pgo "${GO_ARGS[@]}")
fi
go build "${GO_ARGS[@]}"
echo "built bin/server"
