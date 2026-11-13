#!/usr/bin/env bash
# Builds the server as a normal user -> bin/feed, with the compiler and LLVM
# install.sh put in place. Everything the program imports is Nish's standard
# library; SQLite is the amalgamation the compiler vendors.
set -euo pipefail
cd "$(dirname "$0")"
NISH="${NISH:-/opt/nish/bin/nish}"
[ -x "$NISH" ] || NISH=nish   # local development: nish on PATH
export CC="${CC:-clang-18}"
# SQLite's busy handler sleeps whole seconds unless it may call usleep(); with
# it, a writer that finds another process's write lock retries within a
# millisecond.
export NISH_SQLITE_CFLAGS="${NISH_SQLITE_CFLAGS:--DHAVE_USLEEP=1}"
mkdir -p bin build
"$NISH" src/main.ts -o build/ --link bin/feed
