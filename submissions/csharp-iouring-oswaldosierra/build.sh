#!/usr/bin/env bash
# Builds as a normal user: SQLite 3.53.4 from checksummed source with GCC profile-guided
# optimization (trained on a synthetic database, see pgo/train.c), then the Native AOT server.
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_NOLOGO=1
DOTNET="${DOTNET:-dotnet}"

SQLITE=sqlite-autoconf-3530400
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
OUT=obj/sqlite
FLAGS=(-O3 -march=native -fno-plt -DNDEBUG
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1)

if [ ! -f "$OUT/libsqlite3.a" ]; then
  rm -rf "$OUT" && mkdir -p "$OUT"
  curl -fsSL --retry 3 -o "$OUT/$SQLITE.tar.gz" "https://www.sqlite.org/2026/$SQLITE.tar.gz"
  echo "$SQLITE_SHA256  $OUT/$SQLITE.tar.gz" | sha256sum -c -
  tar -xzf "$OUT/$SQLITE.tar.gz" -C "$OUT" --strip-components=1 "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h"

  if [ "${NO_PGO:-0}" = 1 ]; then
    gcc "${FLAGS[@]}" -c "$OUT/sqlite3.c" -o "$OUT/sqlite3.o"
  else
    # Pass 1: instrumented build, run on a throwaway synthetic database.
    gcc "${FLAGS[@]}" -fprofile-generate -fprofile-update=single -fprofile-dir="$PWD/$OUT/pgo" \
      -I"$OUT" "$OUT/sqlite3.c" pgo/train.c -o "$OUT/train" -lm
    "$OUT/train" "$OUT/train.db"
    rm -f "$OUT"/train.db*
    # Pass 2: optimized with the recorded profile.
    gcc "${FLAGS[@]}" -fprofile-use -fprofile-correction -Wno-missing-profile -fprofile-dir="$PWD/$OUT/pgo" \
      -c "$OUT/sqlite3.c" -o "$OUT/sqlite3.o"
  fi
  ar rcs "$OUT/libsqlite3.a" "$OUT/sqlite3.o"
fi

"$DOTNET" publish Server.csproj -c Release -p:RestoreLockedMode=true -o bin
