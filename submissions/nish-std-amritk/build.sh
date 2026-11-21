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
# --allow-panics: since 0.44.0 the compiler refuses a site it cannot prove
# never panics (an index, an overflow); with the flag such a site is checked at
# run time, as every earlier release did.
"$NISH" --allow-panics src/main.ts -o build/ --link bin/feed
