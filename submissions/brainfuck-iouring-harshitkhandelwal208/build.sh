#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps

# 1. Compile native Brainfuck AOT compiler
clang -O3 -o bin/bf_compiler src/compiler/bf_compiler.c

# 2. Compile native Brainfuck server source (src/bf/server.bf) into optimized C
bin/bf_compiler src/bf/server.bf src/server_bf.c

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  if [ -f "../../vendor-cache/$SQLITE.zip" ]; then
    cp "../../vendor-cache/$SQLITE.zip" deps/$SQLITE.zip
  else
    curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  fi
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

zig build -Doptimize=ReleaseFast -Dcpu=native
