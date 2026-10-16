#!/usr/bin/env bash
# Builds as a normal user:
#  1. SQLite from its official source (pinned, SHA-256 checked) into native/libsqlite3.so.
#  2. Oxpecker from delneg/Oxpecker at a pinned commit, minus one experiment (see README).
#  3. The server with dotnet publish (ReadyToRun) into out/.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=3530400   # 3.53.4
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
OXPECKER_REPO=https://github.com/delneg/Oxpecker.git
OXPECKER_COMMIT=ef46b35db7b00a02075435861bd31494265b0437   # branch autoresearch/perf-2026-10-14
OXPECKER_DROP=c911ea1b94676d4ecd575f61acca8fd89435b163     # experiment 11, Compose fast path: slower on x64

if [ ! -f native/libsqlite3.so ]; then
  mkdir -p native
  curl -fsSL -o native/sqlite.tar.gz "https://www.sqlite.org/2026/sqlite-autoconf-$SQLITE.tar.gz"
  echo "$SQLITE_SHA256  native/sqlite.tar.gz" | sha256sum -c -
  tar -xzf native/sqlite.tar.gz -C native --strip-components=1 "sqlite-autoconf-$SQLITE/sqlite3.c"
  # SQLite's recommended compile-time options, but THREADSAFE=2: two connections, each used by one thread at a time.
  gcc -O2 -fPIC -shared -o native/libsqlite3.so native/sqlite3.c \
    -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  rm -f native/sqlite.tar.gz native/sqlite3.c
fi

if [ ! -d oxpecker/.git ]; then
  rm -rf oxpecker
  git init -q oxpecker
  git -C oxpecker fetch -q --depth 20 "$OXPECKER_REPO" "$OXPECKER_COMMIT"
  git -C oxpecker -c advice.detachedHead=false checkout -q FETCH_HEAD
  git -C oxpecker diff "$OXPECKER_DROP~1" "$OXPECKER_DROP" | git -C oxpecker apply -R
fi

export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
dotnet publish Server.fsproj -c Release --use-current-runtime --self-contained false -o out
cp native/libsqlite3.so out/
# Stop the compiler servers, so they do not keep memory on a 2 GB box.
dotnet build-server shutdown > /dev/null 2>&1 || true
