#!/usr/bin/env bash
# Builds as a normal user:
#  1. out/libsqlite3td.so: SQLite 3.54.1's amalgamation (pinned, SHA-256 checked), unmodified. -DSQLITE_PRIVATE= is
#     the amalgamation's own switch for its internal-linkage macro: its B-tree functions (sqlite3Btree*) are then
#     exported, and the server calls them through P/Invoke like the public API. -fno-semantic-interposition and
#     -Bsymbolic keep SQLite's calls among its own functions direct (and inlinable) now that they are exported.
#  2. the server with dotnet publish (ReadyToRun, locked NuGet restore) into out/.
set -euo pipefail
cd "$(dirname "$0")"
DOTNET="${DOTNET:-/usr/local/dotnet/dotnet}"
[ -x "$DOTNET" ] || DOTNET="$(command -v dotnet)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p out deps
if [ ! -f "deps/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "deps/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  deps/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "deps/$SQLITE.zip" -d deps
fi
if [ ! -f out/libsqlite3td.so ] || [ build.sh -nt out/libsqlite3td.so ]; then
  gcc -O3 -march=native -fPIC -shared -fno-semantic-interposition -fno-plt -Wl,-Bsymbolic -Wl,-O1 \
    -DSQLITE_PRIVATE= \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_USE_ALLOCA \
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE \
    -DSQLITE_OMIT_TCL_VARIABLE -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 \
    -o out/libsqlite3td.so "deps/$SQLITE/sqlite3.c" -lm -lpthread
fi
(cd src && "$DOTNET" publish Server.fsproj -c Release --use-current-runtime --self-contained false -o ../out -p:RestoreLockedMode=true)
"$DOTNET" build-server shutdown > /dev/null 2>&1 || true
