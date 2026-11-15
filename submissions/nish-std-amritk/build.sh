#!/usr/bin/env bash
# Builds the server as a normal user -> bin/feed, with the compiler and LLVM
# install.sh put in place. Everything the program imports is Nish's standard
# library; SQLite is the amalgamation the compiler vendors.
set -euo pipefail
cd "$(dirname "$0")"
NISH="${NISH:-/opt/nish/bin/nish}"
[ -x "$NISH" ] || NISH=nish   # local development: nish on PATH
export CC="${CC:-clang-18}"
mkdir -p bin build
"$NISH" src/main.ts -o build/ --link bin/feed
